#pragma once

// Stops PC audio when the AirPods leave this PC, so playback does not carry on
// out of the speakers. Two system-wide passes, independent of windows/screens:
//
//  1. Pause: every Global System Media Transport Controls session that is
//     playing (browsers per tab, Spotify, media apps...) gets TryPauseAsync.
//     A real pause that keeps the position. Never sends media keys (they
//     toggle and can start playback) and never resumes anything.
//  2. Mute: ~1.5 s later, any WASAPI audio session on any active output that
//     is still producing sound (peak meter above silence) is muted: games,
//     players without media controls, web audio. System sounds and this
//     process are skipped.
//
// Only sessions this class muted are unmuted, by restoreMutes(): when the
// AirPods come back, when media starts playing on the PC, or from the tray.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Propods {

class PlaybackSilencer {
public:
    struct Result {
        int paused = 0;
        int muted = 0;
        std::vector<std::wstring> names;  // apps paused or muted, for the notification
    };
    using DoneCallback = std::function<void(const Result&)>;

    void setEnabled(bool enabled) { m_enabled.store(enabled); }
    bool enabled() const { return m_enabled.load(); }
    void setOnDone(DoneCallback cb) { m_onDone = std::move(cb); }
    // Checked after a short delay before acting, so a brief Bluetooth blip
    // that returns the AirPods at once does not pause anything.
    void setStillReleased(std::function<bool()> check) { m_stillReleased = std::move(check); }

    // Runs both passes on a background thread; returns immediately. A run
    // already in progress makes this a no-op.
    void silenceAsync(const char* reason);

    // Unmute what this class muted (still muted). Safe from any thread.
    int restoreMutes(const char* reason);

    bool hasMuted();

private:
    Result silence(const char* reason);
    int pausePlayingSessions(std::vector<std::wstring>& names);
    int muteAudibleSessions(std::vector<std::wstring>& names);

    struct MutedSession {
        std::wstring endpointId;
        std::wstring instanceId;
        std::wstring name;
    };

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;  // guards m_muted
    std::vector<MutedSession> m_muted;
    DoneCallback m_onDone;
    std::function<bool()> m_stillReleased;
};

}
