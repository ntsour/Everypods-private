#pragma once

// Peer protocol v2 for Propods: one v2 session per configured peer, plus the
// passive-acquisition participant role. See docs/peer-link.md and
// docs/peer-mesh-spec.md section 1. All peers are equal (no phone/computer kind).
//
// Every peer listens on the v2 RFCOMM service and dials every configured peer it
// has no link with. The peer with the smaller installationId dials at once, the
// other waits 3 s before its first dial. If two links come up between the same
// pair, both ends keep the one with the smallest (initiatorId, connectionId)
// from HELLO and close the other.
//
// Handshake, as dialler: connect -> HELLO -> peer answers HELLO (echoing our
// initiatorId/connectionId), SNAPSHOT, PING -> we answer SNAPSHOT + PONG and
// PING -> its PONG makes the session ready. As the accepting side we mirror
// Android's server: the dialler's HELLO is answered with our HELLO (echoing its
// initiatorId and connectionId), SNAPSHOT and PING. PING every 10 s; two missed
// responses close the link. Any protocol violation closes that link; the dial
// loop reconnects with backoff.
//
// The explicit handover path (legacy 4-byte packets) is untouched; v2 only
// publishes this PC's ownership snapshot and answers ACQUIRE_* so a peer's
// automatic (case-open) reconnect can proceed while this PC is a peer.

#include "PeerV2Frame.hpp"
#include "PeerV2Json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>

namespace Propods::v2 {

enum class Ownership { Unknown, Held, Released };
enum class AudioUse { Unknown, Idle, Media, Call };

struct LocalState {
    std::optional<std::string> headsetId;  // null when no AirPods configured
    Ownership ownership = Ownership::Unknown;
    AudioUse audioUse = AudioUse::Unknown;
    bool operator==(const LocalState&) const = default;
};

class PeerV2Mesh {
public:
    using StateProvider = std::function<LocalState()>;

    explicit PeerV2Mesh(StateProvider provider);
    ~PeerV2Mesh();

    PeerV2Mesh(const PeerV2Mesh&) = delete;
    PeerV2Mesh& operator=(const PeerV2Mesh&) = delete;

    // Starts the v2 server and a dial loop per peer.
    void start(const std::vector<std::uint64_t>& peers);
    void stop();
    void addPeer(std::uint64_t peer);
    void removePeer(std::uint64_t peer);
    // The peer's Bluetooth link came up: cut the dial backoff short. No-op while
    // a v2 link to it is up.
    void retryNow(std::uint64_t peer);

    // True while a peer holds a PREPARING/IN_PROGRESS reservation on us:
    // that peer is mid-connect, so we must not start a takeover ourselves.
    bool reservationActive();

    // Re-measure local state now and broadcast a SNAPSHOT if it changed
    // (also done every 2 s by the monitor thread).
    void refreshLocalState();

private:
    struct PeerSession;  // per-peer state (dial loop, links), defined in the .cpp
    struct Link;         // one v2 connection to a peer, dialled or accepted

    enum class Phase { Preparing, InProgress, Resolved, Cancelled };
    struct Reservation {
        std::string requestId, requesterId, requesterSessionId, headsetId, membershipDigest, grantId;
        std::map<std::string, std::int64_t> snapshotRevisions;
        Phase phase = Phase::Preparing;
        std::chrono::steady_clock::time_point expires;
    };

    void runPeer(std::shared_ptr<PeerSession> session);
    void runLink(std::shared_ptr<PeerSession> session, std::shared_ptr<Link> link);
    void sessionLoop(PeerSession& s, Link& l);
    void handleFrame(PeerSession& s, Link& l, const Frame& frame);
    void handleAcquisition(PeerSession& s, Link& l, FrameType type, const JsonObject& f);
    void arbitrateLocked(PeerSession& s, Link& l);   // m_mutex held; throws if l loses
    void onInboundSocket(winrt::Windows::Networking::Sockets::StreamSocket socket);
    bool startServer();
    void stopServer();
    void closeLink(Link& l);
    void removeSession(const std::shared_ptr<PeerSession>& s);  // joins its threads; m_mutex not held
    void monitorLoop();

    bool send(Link& l, FrameType type, const JsonObject& fields);
    void broadcastSnapshot();
    JsonObject snapshotLocked();             // m_mutex held
    LocalState measure();
    std::optional<std::vector<std::string>> membershipLocked();          // m_mutex held
    std::optional<std::map<std::string, std::int64_t>> revisionsLocked(); // m_mutex held
    void expireReservationLocked();          // m_mutex held

    StateProvider m_provider;
    std::mutex m_mutex;
    std::map<std::uint64_t, std::shared_ptr<PeerSession>> m_sessions;
    LocalState m_state;
    std::int64_t m_revision = 0;
    bool m_stateInitialised = false;
    std::optional<Reservation> m_reservation;

    winrt::Windows::Devices::Bluetooth::Rfcomm::RfcommServiceProvider m_serviceProvider{nullptr};
    winrt::Windows::Networking::Sockets::StreamSocketListener m_listener{nullptr};

    std::atomic<bool> m_running{false};
    std::thread m_monitor;
    std::mutex m_monitorMutex;
    std::condition_variable m_monitorCv;
};

}  // namespace Propods::v2
