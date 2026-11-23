#include "PeerV2Mesh.hpp"

#include "BluetoothRfcommClient.hpp"
#include "Logger.hpp"
#include "PeerDialPolicy.hpp"
#include "PeerV2Arbitration.hpp"
#include "PeerV2Identity.hpp"
#include "PeerV2Message.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <functional>
#include <thread>

namespace Propods::v2 {

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using Clock = std::chrono::steady_clock;

// Android CrossDevice UUID_CROSS_DEVICE_V2.
constexpr winrt::guid kV2Service{
    0x97ce0b4d, 0x8e2f, 0x4a6d, {0xb6, 0xcd, 0xd6, 0x9d, 0x36, 0x2e, 0x75, 0xac}};
constexpr auto kHandshakeDeadline = std::chrono::seconds{5};
constexpr auto kHeartbeatInterval = std::chrono::seconds{10};
constexpr int kMaxMissedIntervals = 2;
constexpr auto kPreparationTtl = std::chrono::seconds{5};
// Android keeps IN_PROGRESS until the requester's terminal CANCEL. A bound
// here keeps a lost CANCEL from blocking our own takeovers forever.
constexpr auto kInProgressSafety = std::chrono::seconds{60};
// Dial backoff, timeout and serialization: PeerDialPolicy.hpp, dialRfcomm().

const char* ownershipName(Ownership o) {
    switch (o) {
        case Ownership::Held: return "held";
        case Ownership::Released: return "released";
        default: return "unknown";
    }
}
const char* audioUseName(AudioUse a) {
    switch (a) {
        case AudioUse::Idle: return "idle";
        case AudioUse::Media: return "media";
        case AudioUse::Call: return "call";
        default: return "unknown";
    }
}

JsonArray toArray(const std::vector<std::string>& v) {
    JsonArray a;
    for (const auto& s : v) a.emplace_back(s);
    return a;
}

}  // namespace

struct PeerV2Mesh::Link {
    bool dialled = false;  // we opened it (otherwise the peer dialled our v2 service)

    // Connection (guarded by writeMutex for writes).
    std::mutex writeMutex;
    StreamSocket socket{nullptr};
    DataWriter writer{nullptr};

    // Protocol state (guarded by the mesh's m_mutex).
    bool connected = false;
    bool helloSeen = false;
    bool ready = false;
    bool acquireCapable = false;
    std::string connectionId;
    std::string initiatorId;
    std::string remoteInstallationId;
    std::string remoteSessionId;
    Clock::time_point connectedAt{};
    std::string outstandingNonce;
    int missedIntervals = 0;
    Clock::time_point lastHeartbeat{};
    // Latest accepted snapshot from this peer on this link.
    bool hasSnapshot = false;
    std::int64_t snapshotRevision = 0;
};

struct PeerV2Mesh::PeerSession {
    struct InboundWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::uint64_t address = 0;
    std::atomic<bool> running{true};
    std::thread worker;  // dial loop
    std::atomic<bool> wake{false};  // retryNow(): cut the backoff wait short

    // Guarded by the mesh's m_mutex.
    std::string remoteInstallationId;   // last known, survives reconnects
    std::vector<std::shared_ptr<Link>> links;
    std::vector<InboundWorker> inboundWorkers;
};

namespace {

// Sleeps up to `d` in small steps; false if asked to stop meanwhile.
// With `wake`, it also returns true early once `wake` is set (and clears it).
bool sleepWhile(const std::function<bool()>& keepGoing, std::chrono::milliseconds d,
                std::atomic<bool>* wake = nullptr) {
    for (auto left = d; left.count() > 0; left -= std::min(left, std::chrono::milliseconds{100})) {
        if (!keepGoing()) return false;
        if (wake && wake->exchange(false)) return true;
        std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds{100}));
    }
    return keepGoing();
}

}  // namespace

PeerV2Mesh::PeerV2Mesh(StateProvider provider) : m_provider(std::move(provider)) {}

PeerV2Mesh::~PeerV2Mesh() { stop(); }

void PeerV2Mesh::start(const std::vector<std::uint64_t>& peers) {
    if (m_running.exchange(true)) return;
    log::info("PeerV2: starting, installation {} session {}",
        localIdentity().installationId, localIdentity().sessionId);
    startServer();
    for (auto p : peers) addPeer(p);
    m_monitor = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        monitorLoop();
    });
}

void PeerV2Mesh::closeLink(Link& l) {
    std::scoped_lock wl{l.writeMutex};
    if (l.socket) { try { l.socket.Close(); } catch (...) {} }
}

void PeerV2Mesh::stop() {
    if (!m_running.exchange(false)) return;
    stopServer();
    { std::scoped_lock lk{m_monitorMutex}; }
    m_monitorCv.notify_all();
    if (m_monitor.joinable()) m_monitor.join();
    std::vector<std::shared_ptr<PeerSession>> sessions;
    {
        std::scoped_lock lk{m_mutex};
        for (auto& [a, s] : m_sessions) sessions.push_back(s);
        m_sessions.clear();
    }
    for (auto& s : sessions) removeSession(s);
}

void PeerV2Mesh::addPeer(std::uint64_t peer) {
    std::shared_ptr<PeerSession> s;
    {
        std::scoped_lock lk{m_mutex};
        if (!m_running.load() || m_sessions.count(peer)) return;
        s = std::make_shared<PeerSession>();
        s->address = peer;
        m_sessions[peer] = s;
    }
    s->worker = std::thread([this, s] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        runPeer(s);
    });
}

void PeerV2Mesh::retryNow(std::uint64_t peer) {
    std::shared_ptr<PeerSession> s;
    {
        std::scoped_lock lk{m_mutex};
        auto it = m_sessions.find(peer);
        if (it == m_sessions.end() || !it->second->links.empty()) return;
        s = it->second;
    }
    log::info("V2 {:012X} peer Bluetooth link is up; dialling now", peer);
    s->wake.store(true);
}

void PeerV2Mesh::removePeer(std::uint64_t peer) {
    std::shared_ptr<PeerSession> s;
    {
        std::scoped_lock lk{m_mutex};
        auto it = m_sessions.find(peer);
        if (it == m_sessions.end()) return;
        s = it->second;
        m_sessions.erase(it);
    }
    removeSession(s);
}

void PeerV2Mesh::removeSession(const std::shared_ptr<PeerSession>& s) {
    s->running.store(false);
    std::vector<std::shared_ptr<Link>> links;
    {
        std::scoped_lock lk{m_mutex};
        links = s->links;
    }
    for (auto& l : links) closeLink(*l);
    if (s->worker.joinable()) s->worker.join();
    std::vector<PeerSession::InboundWorker> workers;
    {
        std::scoped_lock lk{m_mutex};
        workers = std::move(s->inboundWorkers);
        s->inboundWorkers.clear();
    }
    for (auto& w : workers) if (w.thread.joinable()) w.thread.join();
}

bool PeerV2Mesh::reservationActive() {
    std::scoped_lock lk{m_mutex};
    expireReservationLocked();
    return m_reservation && (m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress);
}

// ── v2 server (every peer listens) ──────────────────────────────────────────

bool PeerV2Mesh::startServer() {
    try {
        m_serviceProvider = RfcommServiceProvider::CreateAsync(RfcommServiceId::FromUuid(kV2Service)).get();
        m_listener = StreamSocketListener{};
        m_listener.ConnectionReceived([this](StreamSocketListener const&,
                                             StreamSocketListenerConnectionReceivedEventArgs const& args) {
            try { onInboundSocket(args.Socket()); } catch (...) {}
        });
        m_listener.BindServiceNameAsync(m_serviceProvider.ServiceId().AsString(),
            SocketProtectionLevel::BluetoothEncryptionWithAuthentication).get();
        m_serviceProvider.StartAdvertising(m_listener);
        log::info("V2 service advertised (peers can dial this PC)");
        return true;
    } catch (const winrt::hresult_error& e) {
        log::warn("V2 service failed to start: 0x{:08X} {}",
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        m_listener = nullptr;
        m_serviceProvider = nullptr;
        return false;
    }
}

void PeerV2Mesh::stopServer() {
    try { if (m_serviceProvider) m_serviceProvider.StopAdvertising(); } catch (...) {}
    try { if (m_listener) m_listener.Close(); } catch (...) {}
    m_serviceProvider = nullptr;
    m_listener = nullptr;
}

void PeerV2Mesh::onInboundSocket(StreamSocket socket) {
    const std::uint64_t address = remoteBluetoothAddress(socket);
    const auto reject = [&](const char* why) {
        log::info("V2 {:012X} inbound link rejected: {}", address, why);
        try { socket.Close(); } catch (...) {}
    };
    if (address == 0) return reject("cannot read the caller's address");
    auto link = std::make_shared<Link>();
    link->dialled = false;
    link->socket = socket;
    link->writer = DataWriter{socket.OutputStream()};
    std::scoped_lock lk{m_mutex};
    if (!m_running.load()) return reject("shutting down");
    const auto it = m_sessions.find(address);
    if (it == m_sessions.end()) return reject("not a configured peer");
    const std::shared_ptr<PeerSession> s = it->second;
    // Reap finished inbound workers so threads do not pile up across reconnects.
    for (auto w = s->inboundWorkers.begin(); w != s->inboundWorkers.end();) {
        if (w->done->load()) {
            if (w->thread.joinable()) w->thread.join();
            w = s->inboundWorkers.erase(w);
        } else {
            ++w;
        }
    }
    s->links.push_back(link);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([this, s, link, done] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        runLink(s, link);
        done->store(true);
    });
    s->inboundWorkers.push_back({std::move(t), done});
    log::info("V2 {:012X} peer dialled our v2 service", address);
}

// ── Local state ─────────────────────────────────────────────────────────────

LocalState PeerV2Mesh::measure() {
    try { return m_provider ? m_provider() : LocalState{}; } catch (...) { return LocalState{}; }
}

void PeerV2Mesh::refreshLocalState() {
    const LocalState now = measure();
    bool changed = false;
    {
        std::scoped_lock lk{m_mutex};
        if (!m_stateInitialised || !(now == m_state)) {
            m_state = now;
            m_stateInitialised = true;
            ++m_revision;  // ownership revision: acquisition bookkeeping excluded (Android PeerSnapshotRevision)
            changed = true;
        }
    }
    if (changed) {
        log::handover("V2      local snapshot rev {}: ownership={} audio={}", m_revision,
            ownershipName(now.ownership), audioUseName(now.audioUse));
        broadcastSnapshot();
    }
}

JsonObject PeerV2Mesh::snapshotLocked() {
    expireReservationLocked();
    Json acquisition(nullptr);
    if (m_reservation && (m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress)) {
        acquisition = Json(JsonObject{
            {"requestId", m_reservation->requestId},
            {"requesterId", m_reservation->requesterId},
            {"phase", m_reservation->phase == Phase::InProgress ? "in_progress" : "preparing"}});
    }
    return JsonObject{
        {"sessionId", localIdentity().sessionId},
        {"revision", m_revision},
        {"headsetId", m_state.headsetId ? Json(*m_state.headsetId) : Json(nullptr)},
        {"ownership", ownershipName(m_state.ownership)},
        {"audioUse", audioUseName(m_state.audioUse)},
        {"lastOwnerId", nullptr},
        {"lastOwnerRevision", nullptr},
        {"acquisition", acquisition}};
}

void PeerV2Mesh::broadcastSnapshot() {
    std::scoped_lock lk{m_mutex};
    const JsonObject snap = snapshotLocked();
    for (auto& [a, s] : m_sessions)
        for (auto& l : s->links)
            if (l->helloSeen) send(*l, FrameType::Snapshot, snap);
}

// ── Transport ───────────────────────────────────────────────────────────────

bool PeerV2Mesh::send(Link& l, FrameType type, const JsonObject& fields) {
    try {
        const auto bytes = encodeFrame(encode(type, fields));
        std::scoped_lock wl{l.writeMutex};
        if (!l.writer) return false;
        l.writer.WriteBytes(winrt::array_view<const std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        l.writer.StoreAsync().get();
        log::debug("V2 send {} {}", frameTypeName(type), writeJson(Json(fields)));
        return true;
    } catch (const std::exception& e) {
        log::warn("V2 send {} failed: {}", frameTypeName(type), e.what());
    } catch (const winrt::hresult_error& e) {
        log::warn("V2 send {} failed: 0x{:08X}", frameTypeName(type),
            static_cast<std::uint32_t>(e.code().value));
    }
    closeLink(l);
    return false;
}

// Dial loop for one peer. We dial every configured peer we have no link with:
// at once if our installationId is the smaller (or the peer's is not known yet),
// otherwise after a 3 s wait before the first dial. Duplicates are settled by
// the HELLO keys (arbitrateLocked), so dialling the same peer as it dials us is safe.
void PeerV2Mesh::runPeer(std::shared_ptr<PeerSession> session) {
    PeerSession& s = *session;
    const auto keepGoing = [this, &s] { return s.running.load() && m_running.load(); };
    int attempt = 0;  // failed dials since the last link (Android retryAttempt)

    const auto delay = firstDialDelay(localIdentity().installationId, knownPeerInstallationId(s.address));
    if (delay.count() > 0) {
        log::info("V2 {:012X} waiting {} ms before the first dial (the peer dials first)", s.address, delay.count());
        if (!sleepWhile(keepGoing, delay)) return;
    }
    s.wake.store(false);  // a retryNow() during the first-dial wait is spent

    while (keepGoing()) {
        bool haveLink;
        { std::scoped_lock lk{m_mutex}; haveLink = !s.links.empty(); }
        if (haveLink) {  // the peer dialled us; nothing to dial
            s.wake.store(false);
            attempt = 0;
            if (!sleepWhile(keepGoing, std::chrono::milliseconds{1000})) return;
            continue;
        }
        try {
            // Same dial as the legacy link: uncached resolve, 8 s timeout, one
            // dial at a time across all peers and both services.
            StreamSocket socket = dialRfcomm(s.address, kV2Service, "V2");
            auto link = std::make_shared<Link>();
            link->dialled = true;
            link->socket = socket;
            link->writer = DataWriter{socket.OutputStream()};
            {
                std::scoped_lock lk{m_mutex};
                if (!keepGoing()) { try { socket.Close(); } catch (...) {} return; }
                s.links.push_back(link);
            }
            attempt = 0;
            s.wake.store(false);
            runLink(session, link);
        } catch (const winrt::hresult_error& e) {
            log::debug("V2 {:012X} dial error 0x{:08X} {}", s.address,
                static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            log::debug("V2 {:012X} dial failed: {}", s.address, e.what());
        }
        const auto backoff = peerRetryDelay(attempt, peerRetryJitter());
        ++attempt;
        log::debug("V2 {:012X} redialling in {} ms (attempt {})", s.address, backoff.count(), attempt);
        if (!sleepWhile(keepGoing, backoff, &s.wake)) return;
    }
}

// Runs one link (dialled or accepted) until it ends, then cleans it up.
void PeerV2Mesh::runLink(std::shared_ptr<PeerSession> session, std::shared_ptr<Link> link) {
    PeerSession& s = *session;
    Link& l = *link;
    try {
        sessionLoop(s, l);
    } catch (const winrt::hresult_error& e) {
        log::debug("V2 {:012X} connection error 0x{:08X} {}", s.address,
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        log::debug("V2 {:012X} session ended: {}", s.address, e.what());
    }
    {
        std::scoped_lock lk{m_mutex};
        if (l.connected) log::handover("V2      peer {:012X} {} link closed", s.address, l.dialled ? "dialled" : "accepted");
        l.connected = l.helloSeen = l.ready = false;
        l.remoteSessionId.clear();
        l.outstandingNonce.clear();
        l.hasSnapshot = false;
        std::erase(s.links, link);
    }
    closeLink(l);
    { std::scoped_lock wl{l.writeMutex}; l.socket = nullptr; l.writer = nullptr; }
}

void PeerV2Mesh::sessionLoop(PeerSession& s, Link& l) {
    StreamSocket socket{nullptr};
    { std::scoped_lock wl{l.writeMutex}; socket = l.socket; }
    DataReader reader{socket.InputStream()};
    reader.InputStreamOptions(InputStreamOptions::Partial);
    FrameDecoder decoder;
    {
        std::scoped_lock lk{m_mutex};
        l.connected = true;
        l.connectedAt = Clock::now();
        if (l.dialled) {
            // The accepting side waits for the dialler's HELLO and answers it.
            l.connectionId = randomUuid();
            const auto& me = localIdentity();
            send(l, FrameType::Hello, JsonObject{
                {"installationId", me.installationId},
                {"sessionId", me.sessionId},
                {"capabilities", JsonArray{Json("acquire-v1"), Json("snapshot")}},
                {"initiatorId", me.installationId},
                {"connectionId", l.connectionId}});
        }
    }
    log::info("V2 {:012X} connected ({})", s.address, l.dialled ? "we dialled, HELLO sent" : "peer dialled, awaiting HELLO");
    std::vector<std::uint8_t> buf(4096);
    while (s.running.load() && m_running.load()) {
        const std::uint32_t n = reader.LoadAsync(4096).get();
        if (n == 0) throw std::runtime_error("peer closed the connection");
        buf.resize(n);
        reader.ReadBytes(winrt::array_view<std::uint8_t>(buf.data(), buf.data() + n));
        for (const auto& frame : decoder.feed(std::span<const std::uint8_t>(buf.data(), n)))
            handleFrame(s, l, frame);
    }
}

// Both ends keep the link with the smallest (initiatorId, connectionId) from HELLO
// (Android CrossDevice.registerV2Connection) and close the others.
void PeerV2Mesh::arbitrateLocked(PeerSession& s, Link& l) {
    std::vector<Link*> candidates;
    std::vector<ConnectionKey> keys;
    for (const auto& other : s.links) {
        if (!other->helloSeen) continue;
        candidates.push_back(other.get());
        keys.push_back({other->initiatorId, other->connectionId});
    }
    const int best = preferredConnection(keys);
    if (best < 0) return;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (static_cast<int>(i) == best) continue;
        log::handover("V2      peer {:012X}: duplicate v2 link, closing the one with the larger key", s.address);
        closeLink(*candidates[i]);
    }
    if (candidates[static_cast<std::size_t>(best)] != &l) throw std::invalid_argument("duplicate_v2_link_loser");
}

// ── Protocol ────────────────────────────────────────────────────────────────

void PeerV2Mesh::handleFrame(PeerSession& s, Link& l, const Frame& frame) {
    const Message m = decode(frame);  // throws on any violation -> link closes
    const JsonObject& f = m.fields;
    log::debug("V2 {:012X} recv {} {}", s.address, frameTypeName(m.type), frame.payload);
    std::scoped_lock lk{m_mutex};
    if (!l.helloSeen) {
        if (m.type != FrameType::Hello) throw std::invalid_argument("hello_required");
        const auto& me = localIdentity();
        const std::string remoteId = str(f, "installationId");
        if (l.dialled) {
            // Our HELLO is echoed back: initiatorId and connectionId must be ours.
            if (str(f, "initiatorId") != me.installationId || str(f, "connectionId") != l.connectionId)
                throw std::invalid_argument("hello_echo_mismatch");
        } else {
            // The dialler's own HELLO (Android's v2 server checks the same):
            // it names itself as the initiator and picks the connectionId.
            if (str(f, "initiatorId") != remoteId) throw std::invalid_argument("invalid_initiator_identity");
            l.connectionId = str(f, "connectionId");
        }
        l.initiatorId = str(f, "initiatorId");
        if (remoteId == me.installationId) throw std::invalid_argument("own_identity");
        for (const auto& [a, other] : m_sessions)
            if (other.get() != &s && other->remoteInstallationId == remoteId)
                throw std::invalid_argument("identity_bound_to_other_peer");
        if (!s.remoteInstallationId.empty() && s.remoteInstallationId != remoteId)
            log::warn("V2 {:012X} installation id changed (app reinstalled?)", s.address);
        s.remoteInstallationId = remoteId;
        l.remoteInstallationId = remoteId;
        rememberPeerInstallationId(s.address, remoteId);  // drives the first-dial order
        l.remoteSessionId = str(f, "sessionId");
        const auto caps = strings(f, "capabilities");
        l.acquireCapable = std::find(caps.begin(), caps.end(), "acquire-v1") != caps.end();
        l.helloSeen = true;
        log::handover("V2      peer {:012X} HELLO installation {} session {} ({})", s.address,
            remoteId, l.remoteSessionId, l.dialled ? "we dialled" : "it dialled");
        arbitrateLocked(s, l);  // throws if another link to this peer is preferred
        if (!l.dialled) {
            send(l, FrameType::Hello, JsonObject{
                {"installationId", me.installationId},
                {"sessionId", me.sessionId},
                {"capabilities", JsonArray{Json("acquire-v1"), Json("snapshot")}},
                {"initiatorId", l.initiatorId},
                {"connectionId", l.connectionId}});
        }
        send(l, FrameType::Snapshot, snapshotLocked());
        l.outstandingNonce = randomUuid();
        l.missedIntervals = 0;
        l.lastHeartbeat = Clock::now();
        send(l, FrameType::Ping, JsonObject{{"nonce", l.outstandingNonce}});
        return;
    }
    switch (m.type) {
        case FrameType::Snapshot:
        case FrameType::Pong: {
            const JsonObject& snap = m.type == FrameType::Pong ? obj(f, "snapshot") : f;
            if (str(snap, "sessionId") != l.remoteSessionId) throw std::invalid_argument("snapshot_session_mismatch");
            const std::int64_t rev = integer(snap, "revision");
            const Json* ownership = field(snap, "ownership");
            l.snapshotRevision = rev;
            l.hasSnapshot = true;
            if (m.type == FrameType::Pong) {
                if (str(f, "nonce") != l.outstandingNonce) break;  // stale/unsolicited PONG: ignore
                l.outstandingNonce.clear();
                l.missedIntervals = 0;
                if (!l.ready) {
                    l.ready = true;
                    log::handover("V2      peer {:012X} ready (snapshot rev {}, ownership {})", s.address,
                        rev, *ownership->string());
                }
            }
            // A HELD snapshot from the requester ends a reservation it was preparing.
            if (m_reservation && m_reservation->phase == Phase::Preparing &&
                m_reservation->requesterId == l.remoteInstallationId && *ownership->string() == "held") {
                m_reservation->phase = Phase::Resolved;
                log::handover("V2      reservation {} resolved: requester holds the AirPods", m_reservation->requestId);
            }
            break;
        }
        case FrameType::Ping: {
            const JsonObject snap = snapshotLocked();
            send(l, FrameType::Snapshot, snap);
            send(l, FrameType::Pong, JsonObject{{"nonce", str(f, "nonce")}, {"snapshot", snap}});
            break;
        }
        case FrameType::AcquireRequest:
        case FrameType::AcquireStart:
        case FrameType::AcquireCancel:
            if (!l.acquireCapable) throw std::invalid_argument("peer_missing_acquire_v1");
            handleAcquisition(s, l, m.type, f);
            break;
        case FrameType::AcquireGrant:
        case FrameType::AcquireDeny:
        case FrameType::AcquireStartAck:
            // We never request; replies addressed to us are stray.
            log::debug("V2 {:012X} ignoring {} (Propods never requests)", s.address, frameTypeName(m.type));
            break;
        case FrameType::RelayData:
            break;  // battery/ANC relays are not used on Windows
        case FrameType::Hello:
            throw std::invalid_argument("duplicate_hello");
    }
}

std::optional<std::vector<std::string>> PeerV2Mesh::membershipLocked() {
    std::vector<std::string> ids{localIdentity().installationId};
    for (const auto& [a, s] : m_sessions) {
        if (s->remoteInstallationId.empty()) return std::nullopt;
        ids.push_back(s->remoteInstallationId);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return std::nullopt;
    return ids;
}

std::optional<std::map<std::string, std::int64_t>> PeerV2Mesh::revisionsLocked() {
    std::map<std::string, std::int64_t> revs{{localIdentity().installationId, m_revision}};
    for (const auto& [a, s] : m_sessions) {
        const Link* ready = nullptr;
        for (const auto& l : s->links)
            if (l->ready && l->hasSnapshot) { ready = l.get(); break; }
        if (!ready || s->remoteInstallationId.empty()) return std::nullopt;
        revs[s->remoteInstallationId] = ready->snapshotRevision;
    }
    return revs;
}

void PeerV2Mesh::expireReservationLocked() {
    if (!m_reservation) return;
    const auto now = Clock::now();
    if ((m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress) &&
        now >= m_reservation->expires) {
        log::handover("V2      reservation {} expired in {}", m_reservation->requestId,
            m_reservation->phase == Phase::Preparing ? "PREPARING" : "IN_PROGRESS");
        m_reservation->phase = Phase::Cancelled;
    }
}

void PeerV2Mesh::handleAcquisition(PeerSession& s, Link& l, FrameType type, const JsonObject& f) {
    const auto& me = localIdentity();
    expireReservationLocked();

    if (type == FrameType::AcquireCancel) {
        if (str(f, "requesterId") != l.remoteInstallationId || str(f, "requesterSessionId") != l.remoteSessionId)
            throw std::invalid_argument("cancel_identity_mismatch");
        if (m_reservation && m_reservation->requestId == str(f, "requestId") &&
            m_reservation->requesterId == l.remoteInstallationId &&
            m_reservation->requesterSessionId == l.remoteSessionId &&
            m_reservation->membershipDigest == str(f, "membershipDigest") &&
            (m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress)) {
            m_reservation->phase = Phase::Cancelled;
            log::handover("V2      reservation {} cancelled by requester", m_reservation->requestId);
        }
        return;
    }

    const std::string requestId = str(f, "requestId");
    const std::string headsetId = str(f, "headsetId");
    const std::vector<std::string> members = strings(f, "membership");
    const std::string digest = str(f, "membershipDigest");
    if (str(f, "requesterId") != l.remoteInstallationId || str(f, "requesterSessionId") != l.remoteSessionId)
        throw std::invalid_argument("requester_identity_mismatch");

    JsonObject reply{{"requestId", requestId}, {"headsetId", headsetId},
        {"requesterId", l.remoteInstallationId}, {"requesterSessionId", l.remoteSessionId},
        {"membership", toArray(members)}, {"membershipDigest", digest},
        {"grantorId", me.installationId}, {"grantorSessionId", me.sessionId}};
    const auto deny = [&](const char* reason) {
        JsonObject r = reply;
        r.emplace_back("reason", reason);
        log::handover("V2      {} {} from {:012X} -> DENY {}", frameTypeName(type), requestId, s.address, reason);
        send(l, FrameType::AcquireDeny, r);
    };

    // Our own status, measured now.
    const bool localOk = m_stateInitialised && m_state.headsetId && *m_state.headsetId == headsetId &&
                         m_state.ownership != Ownership::Unknown;
    const bool localFree = m_state.ownership == Ownership::Released && m_state.audioUse == AudioUse::Idle;

    if (type == FrameType::AcquireRequest) {
        const auto configured = membershipLocked();
        if (!configured || *configured != members || std::find(members.begin(), members.end(), me.installationId) == members.end())
            return deny("MEMBERSHIP_MISMATCH");
        std::map<std::string, std::int64_t> theirs;
        for (const auto& [k, v] : obj(f, "snapshotRevisions")) theirs[k] = *v.integer();
        const auto ours = revisionsLocked();
        if (!ours || *ours != theirs) {
            if (ours) {
                std::string diff;
                for (const auto& [k, v] : theirs) {
                    auto it = ours->find(k);
                    if (it == ours->end() || it->second != v)
                        diff += std::format(" {}:{}vs{}", k.substr(0, 8), v, it == ours->end() ? -1 : it->second);
                }
                log::debug("V2 stale revisions (theirs vs ours):{}", diff);
            }
            return deny("STALE_SNAPSHOT");
        }
        if (!localOk) return deny("UNKNOWN");
        if (!localFree) return deny("HELD");
        if (m_reservation && (m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress)) {
            const auto& r = *m_reservation;
            if (r.requestId == requestId && r.requesterId == l.remoteInstallationId &&
                r.requesterSessionId == l.remoteSessionId && r.headsetId == headsetId && r.membershipDigest == digest) {
                JsonObject g = reply;
                g.emplace_back("grantId", r.grantId);
                send(l, FrameType::AcquireGrant, g);  // duplicate request: same grant
                return;
            }
            return deny("BUSY");
        }
        Reservation r;
        r.requestId = requestId;
        r.requesterId = l.remoteInstallationId;
        r.requesterSessionId = l.remoteSessionId;
        r.headsetId = headsetId;
        r.membershipDigest = digest;
        r.grantId = randomUuid();
        r.snapshotRevisions = theirs;
        r.phase = Phase::Preparing;
        r.expires = Clock::now() + kPreparationTtl;
        m_reservation = r;
        JsonObject g = reply;
        g.emplace_back("grantId", r.grantId);
        log::handover("V2      ACQUIRE_REQUEST {} from {:012X} -> GRANT", requestId, s.address);
        send(l, FrameType::AcquireGrant, g);
        return;
    }

    // ACQUIRE_START
    if (str(f, "grantorId") != me.installationId || str(f, "grantorSessionId") != me.sessionId)
        throw std::invalid_argument("start_identity_mismatch");
    const std::string grantId = str(f, "grantId");
    if (!m_reservation || m_reservation->requestId != requestId ||
        m_reservation->requesterId != l.remoteInstallationId ||
        m_reservation->requesterSessionId != l.remoteSessionId || m_reservation->grantId != grantId)
        return deny("UNKNOWN");  // Android maps EXPIRED to UNKNOWN on the wire
    Reservation& r = *m_reservation;
    const bool duplicate = r.phase == Phase::InProgress;
    if (!duplicate && r.phase != Phase::Preparing) return deny("UNKNOWN");
    const auto ours = revisionsLocked();
    if (!ours || *ours != r.snapshotRevisions) {
        if (!duplicate) r.phase = Phase::Cancelled;
        return deny("STALE_SNAPSHOT");
    }
    const auto configured = membershipLocked();
    if (!configured || *configured != members || membershipDigest(members) != r.membershipDigest) {
        if (!duplicate) r.phase = Phase::Cancelled;
        return deny("MEMBERSHIP_MISMATCH");
    }
    if (!localOk || !localFree) {
        if (!duplicate) r.phase = Phase::Cancelled;
        return deny(localOk ? "HELD" : "UNKNOWN");
    }
    if (!duplicate) {
        r.phase = Phase::InProgress;
        r.expires = Clock::now() + kInProgressSafety;
        log::handover("V2      ACQUIRE_START {} from {:012X} -> START_ACK (peer connecting)", requestId, s.address);
    }
    JsonObject ack = reply;
    ack.emplace_back("grantId", grantId);
    send(l, FrameType::AcquireStartAck, ack);
}

// ── Monitor: local state, heartbeat, handshake deadline ─────────────────────

void PeerV2Mesh::monitorLoop() {
    while (m_running.load()) {
        refreshLocalState();
        {
            std::scoped_lock lk{m_mutex};
            const auto now = Clock::now();
            for (auto& [a, sp] : m_sessions) {
                for (auto& lp : sp->links) {
                    Link& l = *lp;
                    if (!l.connected) continue;
                    bool close = false;
                    if (!l.ready && now - l.connectedAt > kHandshakeDeadline) {
                        log::warn("V2 {:012X} handshake not complete in 5 s; closing", sp->address);
                        close = true;
                    } else if (l.helloSeen && now - l.lastHeartbeat >= kHeartbeatInterval) {
                        l.lastHeartbeat = now;
                        if (!l.outstandingNonce.empty()) {
                            if (++l.missedIntervals >= kMaxMissedIntervals) {
                                log::warn("V2 {:012X} heartbeat response missed twice; closing", sp->address);
                                close = true;
                            }
                        } else {
                            l.outstandingNonce = randomUuid();
                            send(l, FrameType::Ping, JsonObject{{"nonce", l.outstandingNonce}});
                        }
                    }
                    if (close) closeLink(l);
                }
            }
        }
        std::unique_lock lk{m_monitorMutex};
        m_monitorCv.wait_for(lk, std::chrono::seconds{2}, [this] { return !m_running.load(); });
    }
}

}  // namespace Propods::v2
