#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "AirPodsConnector.hpp"
#include "MediaPlaybackWatcher.hpp"
#include "PeerRegistry.hpp"
#include "PlaybackHold.hpp"

namespace Propods {

enum class OwnershipState {
    Unknown,
    LocalPc,
    RemoteAndroid,
};

class HandoverController {
public:
    using StateChangedCallback = std::function<void(OwnershipState)>;

    HandoverController(PeerRegistry& peers,
                       AirPodsConnector& airpods,
                       MediaPlaybackWatcher& media);
    ~HandoverController();

    void setOnStateChanged(StateChangedCallback cb) { m_onStateChanged = std::move(cb); }

    // Optional probe that returns true if the AirPods appear to be out of the
    // case / nearby (BLE proximity). When set and it returns false, a media/call
    // trigger will NOT initiate a BT takeover — this avoids poking the BT stack
    // and hijacking audio from another device (Jabra/Dell) when the AirPods are
    // simply stowed. Does not affect the manual "Connect AirPods now" path.
    void setAvailabilityProbe(std::function<bool()> probe) { m_availabilityProbe = std::move(probe); }

    // True when the AirPods are in use on a peer: a peer's v2 snapshot holds them
    // and a bud is worn (fresh BLE evidence). The playback hold, its pre-mute and
    // the progress panel start only then: otherwise the AirPods may be in their
    // case and the panel would only report a failure. The takeover still runs quietly.
    void setHoldProbe(std::function<bool()> probe) { m_holdProbe = std::move(probe); }

    // When disabled, the controller will NOT automatically set AirPods as the
    // default audio device on takeover. The user can still trigger it manually
    // via the tray menu "Connect AirPods now" item.
    void setAutoRoute(bool enabled) { m_autoRoute.store(enabled); }

    // ── Lid-open connect (the phones' "Auto-connect on case open (last
    // device)"). The phones leave a lid opening to the device that used the
    // AirPods last; when that was this PC, it connects them here.
    //
    // The lease ("this PC used them last") is set when the AirPods come to this
    // PC and kept when they go into the case. It is cleared when a peer claims
    // them (kAirPodsConnected) or this PC releases them to a peer. Persisted by
    // the owner through the change callback.
    void setLidOpenConnect(bool enabled) { m_lidOpenConnect.store(enabled); }
    bool lidOpenConnect() const { return m_lidOpenConnect.load(); }
    void setLidLease(bool held) { m_lidLease.store(held); }  // initial value, no callback
    bool lidLease() const { return m_lidLease.load(); }
    void setOnLidLeaseChanged(std::function<void(bool held)> cb) { m_onLidLeaseChanged = std::move(cb); }
    // True when a peer's v2 snapshot says it holds the AirPods.
    void setPeerHoldsProbe(std::function<bool()> probe) { m_peerHoldsProbe = std::move(probe); }
    // A case lid was opened near this PC (BleProximityWatcher). Any thread.
    void onLidOpened();
    bool autoRoute() const          { return m_autoRoute.load(); }

    // Optional playback hold + progress panel (not owned). Set before start.
    void setPlaybackHold(PlaybackHold* hold) { m_hold = hold; }

    void onMediaPlayingChanged(bool playing);
    // Teams/WASAPI-specific trigger — goes through the voluntary-release gate.
    // Wire TeamsCallWatcher::setOnTriggered to this instead of onMediaPlayingChanged.
    // isCall=false for media apps outside the media controls (VLC).
    void onCallActive(bool isCall = true);

    // Hold panel actions. Retry skips the probe / anti-pingpong / same-app gates
    // (the user asked); cancel stops the remaining connect attempts.
    void retryTakeover(HoldKind kind);
    void cancelTakeover();
    // Cheap check for the hold's pre-mute: would a media start take the AirPods now?
    bool mediaTakeoverLikely();
    // Called when the Teams audio session expires (call truly ended).
    // Clears the voluntary-release gate so the next new call can trigger a claim.
    void onTeamsCallEnded();
    void onIncomingPacket(std::span<const std::uint8_t> data);
    void onPeerConnectionChanged(bool connected);

    OwnershipState state() const { return m_state.load(); }

private:
    // Returns true if the state actually changed (the atomic exchange saw a different
    // previous value). Callers that need to act only on a real transition (e.g., send
    // a protocol packet) should gate on the return value to avoid duplicate sends when
    // two threads race to resolve the same Unknown→X transition.
    bool setState(OwnershipState s);
    bool withinDebounceWindow();
    void requestTakeover(HoldKind kind, bool userRetry, const std::wstring& holdAppId);
    void startAudioWatcher();

    // Helpers to store/load anti-pingpong timestamps atomically.
    // steady_clock::duration::count() is int64_t nanoseconds on all supported platforms.
    static std::int64_t tpToNs(std::chrono::steady_clock::time_point tp) noexcept {
        return tp.time_since_epoch().count();
    }
    static std::chrono::steady_clock::time_point tpFromNs(std::int64_t ns) noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{ns}};
    }

    PeerRegistry& m_peers;
    AirPodsConnector& m_airpods;
    MediaPlaybackWatcher& m_media;

    std::atomic<OwnershipState> m_state{OwnershipState::Unknown};

    std::mutex m_debounceMutex;
    std::chrono::steady_clock::time_point m_lastAction{};
    static constexpr std::chrono::milliseconds kDebounce{300};

    // Anti-ping-pong: timestamp of our last successful local takeover. We reject
    // incoming RequestDisconnect within ~3s of this to avoid Android's auto-takeover
    // logic stealing the AirPods right back.
    // Stored as nanoseconds (steady_clock::duration::count()) so the field can be
    // accessed atomically from both the watcher thread and WinRT/RFCOMM callbacks.
    std::atomic<std::int64_t> m_lastLocalTakeover{0};

    // Sender-side anti-pingpong: when we lose ownership (Android takes over),
    // suppress our own media-start handover for 3 s in case Spotify/Chrome auto-resumes.
    std::atomic<std::int64_t> m_lastLostOwnership{0};

    // Timestamp (nanoseconds, steady_clock) of the last kAirPodsConnected we
    // received from a peer. Used by the falling-edge RECLAIM logic to skip the
    // reclaim when the peer has just intentionally claimed ownership — without
    // this, Windows fights the user's "play media on phone" intent by trying
    // to grab the AirPods back.
    std::atomic<std::int64_t> m_lastPeerOwnershipClaim{0};

    // Event-driven kickoff for the BT connect attempt during Pixel→Windows
    // handover. The takeover thread waits on this condvar (with timeout) for a
    // kAirPodsDisconnected ack from the peer, then starts the BT connect
    // immediately — instead of always sleeping a fixed 1500 ms. Cuts the
    // best-case Pixel→Windows latency by ~1.4 s.
    std::mutex m_peerAckMtx;
    std::condition_variable m_peerAckCv;
    std::atomic<std::int64_t> m_lastPeerAckAt{0};

    // Audio session watcher: background thread that polls for active audio on the
    // AirPods endpoint and signals state changes to connected Android peers.
    std::atomic<bool> m_watcherRunning{false};
    std::thread m_watcherThread;

    // Voluntary-release gate. Set when Windows accepts a peer's kRequestDisconnect
    // or kRequestHandover — i.e. Windows deliberately handed AirPods to another device.
    // While set, onCallActive() (Teams/WASAPI trigger) is suppressed so an ongoing
    // Teams call that is now routing through the peer's AirPods doesn't cause Windows
    // to immediately reclaim them. Cleared when:
    //   - setState(LocalPc): Windows claimed AirPods back by any means
    //   - onTeamsCallEnded(): Teams session expired, a new call should be claimable
    //   - onMediaPlayingChanged(false): media stopped, next user play is genuine intent
    std::atomic<bool> m_voluntarilyReleased{false};

    // Set to true while a BT connect thread is running; cleared when it exits
    // (success or failure). Prevents a second kRequestDisconnect from being sent
    // while a takeover attempt is already in flight. Without this, the connect
    // thread's tryPlayActive() on failure resumes media, GSMTC fires playing=true,
    // and onMediaPlayingChanged immediately spawns another thread — creating a
    // loop that floods peers with REQUEST_DISCONNECT at the connect-backoff cadence.
    std::atomic<bool> m_takeoverInFlight{false};

    void updateLidLease(bool held, const char* why);
    std::atomic<bool> m_lidOpenConnect{true};
    std::atomic<bool> m_lidLease{false};
    std::atomic<bool> m_lidConnectInFlight{false};
    std::function<void(bool)> m_onLidLeaseChanged;
    std::function<bool()> m_peerHoldsProbe;

    // The AirPods mic (hands-free link). Windows sometimes brings the AirPods up
    // with stereo only, e.g. when they reconnect by themselves (2026-11-24: mic
    // "Disconnected" for 33 min, Teams saw no AirPods mic). 8 s after they come
    // up on this PC, check once; if the mic link is down, connect it. The
    // driver briefly reopens stereo too, so while something plays this waits
    // for playback to stop or a call to start. At most 2 tries per connection.
    // One endpoint enumeration per check, never a poll.
    void scheduleHandsFreeCheck(std::chrono::milliseconds delay, bool allowWhilePlaying, const char* reason);
    void checkHandsFree(bool allowWhilePlaying, const char* reason);
    std::atomic<std::uint64_t> m_handsFreeGen{0};  // bumped on every ownership change
    std::atomic<bool> m_handsFreePending{false};    // mic down, waiting for a quiet moment
    std::atomic<int> m_handsFreeAttempts{0};

    // W4 fix: after voluntarily releasing to a peer, suppress GSMTC re-triggers from
    // the same app for 30 s. Prevents YouTube ads and Chrome auto-resume from
    // stealing the AirPods back after the user explicitly moved them to a phone.
    std::atomic<std::int64_t> m_lastVoluntaryReleaseAt{0};
    mutable std::mutex m_voluntaryReleaseAppMutex;
    std::string m_voluntaryReleaseAppId;
    static constexpr std::chrono::seconds kVoluntaryReleaseSuppressWindow{30};

    // A peer reported an active call/meeting (kPeerAudioActive); cleared by
    // kPeerAudioIdle or when the last peer link drops. Suppresses our call takeover.
    std::atomic<bool> m_peerAudioActive{false};

    // Set by cancelTakeover(): the connect thread stops before its next attempt.
    std::atomic<bool> m_cancelTakeover{false};
    PlaybackHold* m_hold{nullptr};

    std::atomic<bool>    m_autoRoute{true};
    StateChangedCallback m_onStateChanged;
    std::function<bool()> m_availabilityProbe;
    std::function<bool()> m_holdProbe;
};

}
