#pragma once

// Peer protocol v2 for Propods: one v2 session per configured phone, plus the
// passive-acquisition participant role. See docs/windows-peer-protocol-v2-design.md.
//
// Propods always dials (phones never dial a computer). Per phone:
//   connect to the v2 RFCOMM service -> HELLO -> phone answers HELLO, SNAPSHOT,
//   PING -> we answer SNAPSHOT + PONG and PING -> its PONG makes the session
//   ready. PING every 10 s; two missed responses close the link. Any protocol
//   violation closes that phone's link; the loop reconnects with backoff.
//
// The explicit handover path (legacy 4-byte packets) is untouched; v2 only
// publishes this PC's ownership snapshot and answers ACQUIRE_* so a phone's
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

    void start(const std::vector<std::uint64_t>& phones);
    void stop();
    void addPeer(std::uint64_t phone);
    void removePeer(std::uint64_t phone);

    // True while a phone holds a PREPARING/IN_PROGRESS reservation on us:
    // that phone is mid-connect, so we must not start a takeover ourselves.
    bool reservationActive();

    // Re-measure local state now and broadcast a SNAPSHOT if it changed
    // (also done every 2 s by the monitor thread).
    void refreshLocalState();

private:
    struct PeerSession;  // per-phone connection state, defined in the .cpp

    enum class Phase { Preparing, InProgress, Resolved, Cancelled };
    struct Reservation {
        std::string requestId, requesterId, requesterSessionId, headsetId, membershipDigest, grantId;
        std::map<std::string, std::int64_t> snapshotRevisions;
        Phase phase = Phase::Preparing;
        std::chrono::steady_clock::time_point expires;
    };

    void runPeer(std::shared_ptr<PeerSession> session);
    void sessionLoop(PeerSession& s);
    void handleFrame(PeerSession& s, const Frame& frame);
    void handleAcquisition(PeerSession& s, FrameType type, const JsonObject& f);
    void monitorLoop();

    bool send(PeerSession& s, FrameType type, const JsonObject& fields);
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

    std::atomic<bool> m_running{false};
    std::thread m_monitor;
    std::mutex m_monitorMutex;
    std::condition_variable m_monitorCv;
};

}  // namespace Propods::v2
