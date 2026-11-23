#pragma once

// Per-app (WASAPI audio session) mute used by PlaybackHold to keep the PC
// speakers quiet for the first moments of a handover, before the playing app
// has actually paused. Only the session(s) of the app that started playing are
// muted, never the whole output, and never for longer than PlaybackHold's cap.
//
// Windows remembers per-app mute across restarts, so every mute is written to
// %APPDATA%\Propods\hold-muted-apps.txt BEFORE SetMute is called, and
// restoreLeftovers() undoes anything left behind by a crash or kill at the next
// start. Only sessions this class muted are ever unmuted; a session the user
// muted themselves is never touched.
//
// Thread safety: every method may be called from any thread. Methods that
// touch COM initialise it themselves (MTA) and tolerate an already-initialised
// apartment.

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Propods {

// One audio session on one output device.
struct AudioSessionRef {
    std::wstring endpointId;  // IMMDevice::GetId of the render endpoint
    std::wstring instanceId;  // IAudioSessionControl2::GetSessionInstanceIdentifier
    DWORD        pid = 0;
    std::wstring exe;         // process image basename, e.g. L"chrome.exe"
};

class SessionMute {
public:
    SessionMute();

    // Mute this one session unless it is already muted (by the user or by us).
    // The session is recorded and the recovery file written before SetMute.
    // Returns true if this call muted it.
    bool mute(const AudioSessionRef& session, const char* reason);

    // Mute every Active, non-system session on the current default render
    // endpoint (eMultimedia) whose exe basename satisfies `matchExe`, skipping
    // this process and sessions already muted. Returns how many were muted.
    int muteActiveSessionsOf(const std::function<bool(const std::wstring& exe)>& matchExe,
                             const char* reason);

    // Unmute every session this class muted that is still muted, then clear
    // the record and the recovery file. Returns how many were unmuted.
    int restoreAll(const char* reason);

    bool anyMuted();

    // At startup: unmute sessions of the apps recorded in the recovery file by
    // an earlier run (matched by executable path, since session ids change
    // when the app restarts), then delete the file. Returns how many.
    int restoreLeftovers();

private:
    struct Muted {
        std::wstring endpointId;
        std::wstring instanceId;
        std::wstring exe;
        std::wstring path;  // full executable path, to find the app after a restart
    };
    void saveLocked();  // m_mutex held: rewrite or delete the recovery file

    std::mutex m_mutex;
    std::vector<Muted> m_muted;
};

}
