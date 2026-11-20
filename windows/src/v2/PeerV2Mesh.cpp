#include "PeerV2Mesh.hpp"

#include "Logger.hpp"
#include "PeerV2Identity.hpp"
#include "PeerV2Message.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>

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
constexpr auto kInitialBackoff = std::chrono::seconds{2};
constexpr auto kMaxBackoff = std::chrono::seconds{60};

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

struct PeerV2Mesh::PeerSession {
    std::uint64_t address = 0;
    std::atomic<bool> running{true};
    std::thread worker;

    // Connection (guarded by writeMutex for writes; replaced by the worker).
    std::mutex writeMutex;
    StreamSocket socket{nullptr};
    DataWriter writer{nullptr};

    // Protocol state (guarded by the mesh's m_mutex).
    bool connected = false;
    bool helloSeen = false;
    bool ready = false;
    bool acquireCapable = false;
    std::string connectionId;
    std::string remoteInstallationId;   // last known, survives reconnects
    std::string remoteSessionId;
    Clock::time_point connectedAt{};
    std::string outstandingNonce;
    int missedIntervals = 0;
    Clock::time_point lastHeartbeat{};
    // Latest accepted snapshot from this phone.
    bool hasSnapshot = false;
    std::int64_t snapshotRevision = 0;
};

PeerV2Mesh::PeerV2Mesh(StateProvider provider) : m_provider(std::move(provider)) {}

PeerV2Mesh::~PeerV2Mesh() { stop(); }

void PeerV2Mesh::start(const std::vector<std::uint64_t>& phones) {
    if (m_running.exchange(true)) return;
    log::info("PeerV2: starting, installation {} session {}",
        localIdentity().installationId, localIdentity().sessionId);
    for (auto p : phones) addPeer(p);
    m_monitor = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        monitorLoop();
    });
}

void PeerV2Mesh::stop() {
    if (!m_running.exchange(false)) return;
    { std::scoped_lock lk{m_monitorMutex}; }
    m_monitorCv.notify_all();
    if (m_monitor.joinable()) m_monitor.join();
    std::vector<std::shared_ptr<PeerSession>> sessions;
    {
        std::scoped_lock lk{m_mutex};
        for (auto& [a, s] : m_sessions) sessions.push_back(s);
        m_sessions.clear();
    }
    for (auto& s : sessions) {
        s->running.store(false);
        { std::scoped_lock wl{s->writeMutex}; if (s->socket) { try { s->socket.Close(); } catch (...) {} } }
        if (s->worker.joinable()) s->worker.join();
    }
}

void PeerV2Mesh::addPeer(std::uint64_t phone) {
    std::shared_ptr<PeerSession> s;
    {
        std::scoped_lock lk{m_mutex};
        if (!m_running.load() || m_sessions.count(phone)) return;
        s = std::make_shared<PeerSession>();
        s->address = phone;
        m_sessions[phone] = s;
    }
    s->worker = std::thread([this, s] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        runPeer(s);
    });
}

void PeerV2Mesh::removePeer(std::uint64_t phone) {
    std::shared_ptr<PeerSession> s;
    {
        std::scoped_lock lk{m_mutex};
        auto it = m_sessions.find(phone);
        if (it == m_sessions.end()) return;
        s = it->second;
        m_sessions.erase(it);
    }
    s->running.store(false);
    { std::scoped_lock wl{s->writeMutex}; if (s->socket) { try { s->socket.Close(); } catch (...) {} } }
    if (s->worker.joinable()) s->worker.join();
}

bool PeerV2Mesh::reservationActive() {
    std::scoped_lock lk{m_mutex};
    expireReservationLocked();
    return m_reservation && (m_reservation->phase == Phase::Preparing || m_reservation->phase == Phase::InProgress);
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
        if (s->helloSeen) send(*s, FrameType::Snapshot, snap);
}

// ── Transport ───────────────────────────────────────────────────────────────

bool PeerV2Mesh::send(PeerSession& s, FrameType type, const JsonObject& fields) {
    try {
        const auto bytes = encodeFrame(encode(type, fields));
        std::scoped_lock wl{s.writeMutex};
        if (!s.writer) return false;
        s.writer.WriteBytes(winrt::array_view<const std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        s.writer.StoreAsync().get();
        log::debug("V2 {:012X} send {} {}", s.address, frameTypeName(type), writeJson(Json(fields)));
        return true;
    } catch (const std::exception& e) {
        log::warn("V2 {:012X} send {} failed: {}", s.address, frameTypeName(type), e.what());
    } catch (const winrt::hresult_error& e) {
        log::warn("V2 {:012X} send {} failed: 0x{:08X}", s.address, frameTypeName(type),
            static_cast<std::uint32_t>(e.code().value));
    }
    { std::scoped_lock wl{s.writeMutex}; if (s.socket) { try { s.socket.Close(); } catch (...) {} } }
    return false;
}

void PeerV2Mesh::runPeer(std::shared_ptr<PeerSession> session) {
    PeerSession& s = *session;
    auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(kInitialBackoff);
    while (s.running.load() && m_running.load()) {
        try {
            auto device = BluetoothDevice::FromBluetoothAddressAsync(s.address).get();
            if (!device) throw std::runtime_error("phone not paired");
            auto services = device.GetRfcommServicesForIdAsync(RfcommServiceId::FromUuid(kV2Service),
                BluetoothCacheMode::Uncached).get();
            if (services.Services().Size() == 0) throw std::runtime_error("v2 service not offered");
            auto svc = services.Services().GetAt(0);
            StreamSocket socket;
            socket.ConnectAsync(svc.ConnectionHostName(), svc.ConnectionServiceName()).get();
            {
                std::scoped_lock wl{s.writeMutex};
                s.socket = socket;
                s.writer = DataWriter{socket.OutputStream()};
            }
            backoff = std::chrono::duration_cast<std::chrono::milliseconds>(kInitialBackoff);
            sessionLoop(s);
        } catch (const winrt::hresult_error& e) {
            log::debug("V2 {:012X} connection error 0x{:08X} {}", s.address,
                static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            log::debug("V2 {:012X} session ended: {}", s.address, e.what());
        }
        {
            std::scoped_lock lk{m_mutex};
            if (s.connected) log::handover("V2      phone {:012X} session closed", s.address);
            s.connected = s.helloSeen = s.ready = false;
            s.remoteSessionId.clear();
            s.outstandingNonce.clear();
            s.hasSnapshot = false;
        }
        {
            std::scoped_lock wl{s.writeMutex};
            if (s.socket) { try { s.socket.Close(); } catch (...) {} }
            s.socket = nullptr;
            s.writer = nullptr;
        }
        for (int i = 0; i < 10 && s.running.load() && m_running.load(); ++i)
            std::this_thread::sleep_for(backoff / 10);
        backoff = std::min(backoff * 2, std::chrono::duration_cast<std::chrono::milliseconds>(kMaxBackoff));
    }
}

void PeerV2Mesh::sessionLoop(PeerSession& s) {
    StreamSocket socket{nullptr};
    { std::scoped_lock wl{s.writeMutex}; socket = s.socket; }
    DataReader reader{socket.InputStream()};
    reader.InputStreamOptions(InputStreamOptions::Partial);
    FrameDecoder decoder;
    {
        std::scoped_lock lk{m_mutex};
        s.connected = true;
        s.connectedAt = Clock::now();
        s.connectionId = randomUuid();
        const auto& me = localIdentity();
        send(s, FrameType::Hello, JsonObject{
            {"installationId", me.installationId},
            {"sessionId", me.sessionId},
            {"capabilities", JsonArray{Json("acquire-v1"), Json("snapshot")}},
            {"initiatorId", me.installationId},
            {"connectionId", s.connectionId}});
    }
    log::info("V2 {:012X} connected, HELLO sent", s.address);
    std::vector<std::uint8_t> buf(4096);
    while (s.running.load() && m_running.load()) {
        const std::uint32_t n = reader.LoadAsync(4096).get();
        if (n == 0) throw std::runtime_error("phone closed the connection");
        buf.resize(n);
        reader.ReadBytes(winrt::array_view<std::uint8_t>(buf.data(), buf.data() + n));
        for (const auto& frame : decoder.feed(std::span<const std::uint8_t>(buf.data(), n)))
            handleFrame(s, frame);
    }
}

// ── Protocol ────────────────────────────────────────────────────────────────

void PeerV2Mesh::handleFrame(PeerSession& s, const Frame& frame) {
    const Message m = decode(frame);  // throws on any violation -> link closes
    const JsonObject& f = m.fields;
    log::debug("V2 {:012X} recv {} {}", s.address, frameTypeName(m.type), frame.payload);
    std::scoped_lock lk{m_mutex};
    if (!s.helloSeen) {
        if (m.type != FrameType::Hello) throw std::invalid_argument("hello_required");
        const auto& me = localIdentity();
        if (str(f, "initiatorId") != me.installationId || str(f, "connectionId") != s.connectionId)
            throw std::invalid_argument("hello_echo_mismatch");
        const std::string remoteId = str(f, "installationId");
        if (remoteId == me.installationId) throw std::invalid_argument("own_identity");
        for (const auto& [a, other] : m_sessions)
            if (other.get() != &s && other->remoteInstallationId == remoteId)
                throw std::invalid_argument("identity_bound_to_other_phone");
        if (!s.remoteInstallationId.empty() && s.remoteInstallationId != remoteId)
            log::warn("V2 {:012X} installation id changed (app reinstalled?)", s.address);
        s.remoteInstallationId = remoteId;
        s.remoteSessionId = str(f, "sessionId");
        const auto caps = strings(f, "capabilities");
        s.acquireCapable = std::find(caps.begin(), caps.end(), "acquire-v1") != caps.end();
        s.helloSeen = true;
        log::handover("V2      phone {:012X} HELLO installation {} session {}", s.address,
            remoteId, s.remoteSessionId);
        send(s, FrameType::Snapshot, snapshotLocked());
        s.outstandingNonce = randomUuid();
        s.missedIntervals = 0;
        s.lastHeartbeat = Clock::now();
        send(s, FrameType::Ping, JsonObject{{"nonce", s.outstandingNonce}});
        return;
    }
    switch (m.type) {
        case FrameType::Snapshot:
        case FrameType::Pong: {
            const JsonObject& snap = m.type == FrameType::Pong ? obj(f, "snapshot") : f;
            if (str(snap, "sessionId") != s.remoteSessionId) throw std::invalid_argument("snapshot_session_mismatch");
            const std::int64_t rev = integer(snap, "revision");
            const Json* ownership = field(snap, "ownership");
            s.snapshotRevision = rev;
            s.hasSnapshot = true;
            if (m.type == FrameType::Pong) {
                if (str(f, "nonce") != s.outstandingNonce) break;  // stale/unsolicited PONG: ignore
                s.outstandingNonce.clear();
                s.missedIntervals = 0;
                if (!s.ready) {
                    s.ready = true;
                    log::handover("V2      phone {:012X} ready (snapshot rev {}, ownership {})", s.address,
                        rev, *ownership->string());
                }
            }
            // A HELD snapshot from the requester ends a reservation it was preparing.
            if (m_reservation && m_reservation->phase == Phase::Preparing &&
                m_reservation->requesterId == s.remoteInstallationId && *ownership->string() == "held") {
                m_reservation->phase = Phase::Resolved;
                log::handover("V2      reservation {} resolved: requester holds the AirPods", m_reservation->requestId);
            }
            break;
        }
        case FrameType::Ping: {
            const JsonObject snap = snapshotLocked();
            send(s, FrameType::Snapshot, snap);
            send(s, FrameType::Pong, JsonObject{{"nonce", str(f, "nonce")}, {"snapshot", snap}});
            break;
        }
        case FrameType::AcquireRequest:
        case FrameType::AcquireStart:
        case FrameType::AcquireCancel:
            if (!s.acquireCapable) throw std::invalid_argument("peer_missing_acquire_v1");
            handleAcquisition(s, m.type, f);
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
        if (!s->ready || !s->hasSnapshot || s->remoteInstallationId.empty()) return std::nullopt;
        revs[s->remoteInstallationId] = s->snapshotRevision;
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

void PeerV2Mesh::handleAcquisition(PeerSession& s, FrameType type, const JsonObject& f) {
    const auto& me = localIdentity();
    expireReservationLocked();

    if (type == FrameType::AcquireCancel) {
        if (str(f, "requesterId") != s.remoteInstallationId || str(f, "requesterSessionId") != s.remoteSessionId)
            throw std::invalid_argument("cancel_identity_mismatch");
        if (m_reservation && m_reservation->requestId == str(f, "requestId") &&
            m_reservation->requesterId == s.remoteInstallationId &&
            m_reservation->requesterSessionId == s.remoteSessionId &&
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
    if (str(f, "requesterId") != s.remoteInstallationId || str(f, "requesterSessionId") != s.remoteSessionId)
        throw std::invalid_argument("requester_identity_mismatch");

    JsonObject reply{{"requestId", requestId}, {"headsetId", headsetId},
        {"requesterId", s.remoteInstallationId}, {"requesterSessionId", s.remoteSessionId},
        {"membership", toArray(members)}, {"membershipDigest", digest},
        {"grantorId", me.installationId}, {"grantorSessionId", me.sessionId}};
    const auto deny = [&](const char* reason) {
        JsonObject r = reply;
        r.emplace_back("reason", reason);
        log::handover("V2      {} {} from {:012X} -> DENY {}", frameTypeName(type), requestId, s.address, reason);
        send(s, FrameType::AcquireDeny, r);
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
            if (r.requestId == requestId && r.requesterId == s.remoteInstallationId &&
                r.requesterSessionId == s.remoteSessionId && r.headsetId == headsetId && r.membershipDigest == digest) {
                JsonObject g = reply;
                g.emplace_back("grantId", r.grantId);
                send(s, FrameType::AcquireGrant, g);  // duplicate request: same grant
                return;
            }
            return deny("BUSY");
        }
        Reservation r;
        r.requestId = requestId;
        r.requesterId = s.remoteInstallationId;
        r.requesterSessionId = s.remoteSessionId;
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
        send(s, FrameType::AcquireGrant, g);
        return;
    }

    // ACQUIRE_START
    if (str(f, "grantorId") != me.installationId || str(f, "grantorSessionId") != me.sessionId)
        throw std::invalid_argument("start_identity_mismatch");
    const std::string grantId = str(f, "grantId");
    if (!m_reservation || m_reservation->requestId != requestId ||
        m_reservation->requesterId != s.remoteInstallationId ||
        m_reservation->requesterSessionId != s.remoteSessionId || m_reservation->grantId != grantId)
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
        log::handover("V2      ACQUIRE_START {} from {:012X} -> START_ACK (phone connecting)", requestId, s.address);
    }
    JsonObject ack = reply;
    ack.emplace_back("grantId", grantId);
    send(s, FrameType::AcquireStartAck, ack);
}

// ── Monitor: local state, heartbeat, handshake deadline ─────────────────────

void PeerV2Mesh::monitorLoop() {
    while (m_running.load()) {
        refreshLocalState();
        {
            std::scoped_lock lk{m_mutex};
            const auto now = Clock::now();
            for (auto& [a, sp] : m_sessions) {
                PeerSession& s = *sp;
                if (!s.connected) continue;
                bool close = false;
                if (!s.ready && now - s.connectedAt > kHandshakeDeadline) {
                    log::warn("V2 {:012X} handshake not complete in 5 s; closing", s.address);
                    close = true;
                } else if (s.helloSeen && now - s.lastHeartbeat >= kHeartbeatInterval) {
                    s.lastHeartbeat = now;
                    if (!s.outstandingNonce.empty()) {
                        if (++s.missedIntervals >= kMaxMissedIntervals) {
                            log::warn("V2 {:012X} heartbeat response missed twice; closing", s.address);
                            close = true;
                        }
                    } else {
                        s.outstandingNonce = randomUuid();
                        send(s, FrameType::Ping, JsonObject{{"nonce", s.outstandingNonce}});
                    }
                }
                if (close) {
                    std::scoped_lock wl{s.writeMutex};
                    if (s.socket) { try { s.socket.Close(); } catch (...) {} }
                }
            }
        }
        std::unique_lock lk{m_monitorMutex};
        m_monitorCv.wait_for(lk, std::chrono::seconds{2}, [this] { return !m_running.load(); });
    }
}

}  // namespace Propods::v2
