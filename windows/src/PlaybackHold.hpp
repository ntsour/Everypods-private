#pragma once

// PlaybackHold: holds this PC's audio while the AirPods connect, so nothing
// plays from the speakers and playback starts once, on the AirPods.
//
//   1. pre-mute: the moment a media app's audio session goes Active on the
//      speakers and a takeover is likely, that app's session is muted
//      (SessionMute), for at most 1.5 s.
//   2. start(media): the takeover passed every gate. The exact media session
//      that started playing (GSMTC) is paused with TryPauseAsync; the mute is
//      lifted as soon as the app reports Paused (or after the 1.5 s cap).
//   3. Phases: RequestingRelease -> Connecting -> Routing -> Connected.
//   4. Connected = the AirPods stereo endpoint is the default output. 300 ms
//      later the same session gets TryPlayAsync (plus one safety replay).
//   5. Failure: the media stays paused; the overlay offers Retry / Play on PC.
//
// Calls (HoldKind::Call) get the same status for the overlay but are never
// muted or paused. Mirrors the Android and Mac designs (docs/playback-hold.md
// in each repo). All logic runs on one private worker thread; every public
// method is safe from any thread and returns immediately.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "SessionMute.hpp"

namespace Propods {

class MediaPlaybackWatcher;

enum class HoldPhase { RequestingRelease, Connecting, Routing, Connected, Failed };

// Media: audio is held while connecting. Call: progress only, nothing is held.
enum class HoldKind { Media, Call };

struct HoldStatus {
    using Clock = std::chrono::steady_clock;

    HoldPhase    phase = HoldPhase::Connecting;
    HoldKind     kind = HoldKind::Media;
    std::wstring deviceName;  // e.g. L"Nikos's AirPods Pro"
    int          attempt = 1; // 1-based; incremented by Retry
    Clock::time_point phaseStartedAt{};
    Clock::time_point attemptStartedAt{};
    std::chrono::milliseconds attemptTimeout{15000};

    // Overall progress 0..1 for a progress bar. Time-based within phase bands
    // so it always moves: RequestingRelease 0-0.3, Connecting 0.3-0.7,
    // Routing 0.7-0.95, Connected 1, Failed 0.95.
    double progress(Clock::time_point now = Clock::now()) const {
        double lo = 0, hi = 0;
        switch (phase) {
            case HoldPhase::RequestingRelease: lo = 0.0; hi = 0.3; break;
            case HoldPhase::Connecting:        lo = 0.3; hi = 0.7; break;
            case HoldPhase::Routing:           lo = 0.7; hi = 0.95; break;
            case HoldPhase::Connected:         return 1.0;
            case HoldPhase::Failed:            return 0.95;
        }
        const double remaining = (std::max)(0.001,
            std::chrono::duration<double>(attemptStartedAt + attemptTimeout - phaseStartedAt).count());
        double t = std::chrono::duration<double>(now - phaseStartedAt).count() / remaining;
        t = (std::min)((std::max)(t, 0.0), 1.0);
        const double eased = 1.0 - (1.0 - t) * (1.0 - t);
        return lo + (hi - lo) * eased;
    }
};

// What the overlay may call. Implementations must return immediately and are
// safe to call from the UI thread.
class PlaybackHoldActions {
public:
    virtual ~PlaybackHoldActions() = default;
    // Failed -> try connecting again, keeping the hold.
    virtual void retry() = 0;
    // Give up on the AirPods: media resumes on the PC speakers ("Play on PC");
    // a call stays on the PC ("Use PC").
    virtual void playHere() = 0;
    // Close button on a failure: end the hold, leave the media paused.
    virtual void dismiss() = 0;
};

class PlaybackHold : public PlaybackHoldActions {
public:
    using StatusCallback = std::function<void(const std::optional<HoldStatus>&)>;

    struct Hooks {
        std::function<std::wstring()>  deviceName;               // AirPods display name
        std::function<bool()>          airPodsAreDefaultOutput;  // AirPods stereo endpoint is default render + ACTIVE
        std::function<bool()>          preMuteAllowed;           // a media takeover is likely right now
        std::function<void(HoldKind)>  retryTakeover;            // run the takeover again (user asked)
        std::function<void()>          cancelTakeover;           // stop remaining connect attempts
    };

    PlaybackHold(MediaPlaybackWatcher& media, SessionMute& mute);
    ~PlaybackHold() override;

    PlaybackHold(const PlaybackHold&) = delete;
    PlaybackHold& operator=(const PlaybackHold&) = delete;

    void setHooks(Hooks hooks);
    // Called on the worker thread whenever the status changes (nullopt = no hold / hide).
    void setOnStatusChange(StatusCallback cb);

    // "Hold audio while connecting" setting. Off: media is not held, but the
    // panel still shows progress.
    void setEnabled(bool enabled) { m_enabled.store(enabled); }
    bool enabled() const { return m_enabled.load(); }

    // ---- Signals (any thread, non-blocking) ----

    // An audio session went Active on a render endpoint (from TeamsCallWatcher).
    void onRenderSessionActive(const AudioSessionRef& session);
    // A GSMTC session's own playing state changed (from MediaPlaybackWatcher).
    void onSessionPlaybackChanged(const std::wstring& appId, bool playing);

    // The takeover passed all gates and is starting: the panel shows progress
    // for every takeover. appId: the GSMTC session that started playing (empty
    // for calls / apps outside the media controls). holdAudio: also mute and
    // pause that media while connecting (needs the setting on); false shows
    // progress only.
    void start(HoldKind kind, bool peerAsked, const std::wstring& appId, bool holdAudio = true);
    // A peer confirmed it let go (kAirPodsDisconnected) -> Connecting.
    void onPeerReleased();
    // The Bluetooth link to the AirPods is up -> Routing.
    void onConnected();
    // Every connect attempt failed -> Failed.
    void onConnectFailed();
    // The AirPods went to a peer / we released them: end quietly, media stays paused.
    void onLost(const char* reason);
    // The call ended: drop a call status.
    void onCallEnded();

    // ---- Queries (any thread) ----

    // A hold (any phase but Connected) is in progress.
    bool isActive() const { return m_active.load(); }
    // True for a few seconds after Play on PC / a user resume, so the app's
    // own resume is not taken as a new request for the AirPods.
    bool suppressesMediaTrigger() const;

    // Restore every mute now (app quitting). Media paused by a hold stays paused.
    void shutdown();

    // ---- PlaybackHoldActions ----
    void retry() override;
    void playHere() override;
    void dismiss() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_active{false};
    friend class Impl;
};

}
