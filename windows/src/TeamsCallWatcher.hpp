#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include "SessionMute.hpp"
#include "TriggerApps.hpp"

namespace Propods {

// Viability spike: detect Teams calls/meetings via WASAPI session state changes.
//
// Strategy: watch for AudioSessionStateActive on any Teams audio session.
// Additionally, every non-system session on a render endpoint (any app) is
// watched so that setOnRenderSessionActive() can report "this app just started
// playing" -- purely event driven, no polling and no audio streams opened.
// OnSessionCreated catches new sessions; existing sessions (created before we
// started) are enumerated at registration time and also watched for state changes.
// This covers both incoming calls (ringtone makes session Active immediately)
// and meeting joins (existing session transitions to Active when audio flows).
//
// Runs on a dedicated STA thread — required by RegisterSessionNotification.
class TeamsCallWatcher {
public:
    TeamsCallWatcher();
    ~TeamsCallWatcher();

    TeamsCallWatcher(const TeamsCallWatcher&) = delete;
    TeamsCallWatcher& operator=(const TeamsCallWatcher&) = delete;

    // Called when a trigger app's audio session becomes active (see
    // TriggerApps.hpp: Teams, VLC and the apps the user added). `comms` is true
    // for a call app, false for a media app.
    // Wire this to HandoverController::onCallActive() so the WASAPI-based
    // trigger feeds into the handover state machine with voluntary-release gating.
    using TriggerCallback = std::function<void(bool comms)>;
    void setOnTriggered(TriggerCallback cb) { m_onTriggered = std::move(cb); }

    // Called when any non-system audio session (any app, not just watched ones)
    // on a render endpoint changes to Active. Called on a WASAPI callback thread:
    // the receiver must return quickly and must not call back into this class.
    // Not called for sessions that were already Active when we registered.
    // Like the other callbacks, set it before start().
    using RenderSessionActiveCallback = std::function<void(const AudioSessionRef&)>;
    void setOnRenderSessionActive(RenderSessionActiveCallback cb) { m_onRenderSessionActive = std::move(cb); }

    // Called when the active Teams session expires (call definitively ended).
    // Wire this to HandoverController::onTeamsCallEnded() to clear the
    // voluntary-release gate so the next new call can claim AirPods.
    using SessionEndedCallback = std::function<void()>;
    void setOnSessionEnded(SessionEndedCallback cb) { m_onSessionEnded = std::move(cb); }

    // Called when a call app (Teams or one the user added) opens its mic (any
    // of its capture sessions Active: a call or meeting) or closes it (none
    // Active). Raw edges: subscriptions
    // are dropped and re-made on every default-device change, so the receiver
    // should debounce the close. Called on a WASAPI or the watcher's STA
    // thread; return quickly. Set before start().
    using CallMicCallback = std::function<void(bool open)>;
    void setOnCallMicChanged(CallMicCallback cb) { m_onCallMicChanged = std::move(cb); }

    bool start();
    void stop();

    // The apps that raise a takeover. Change it at any time (thread-safe);
    // the next session state change uses the new list.
    TriggerAppList& triggerApps() { return m_apps; }

    // An app with an audio session on a watched endpoint since start (not
    // this process), most recently active first: what the tray offers to add.
    struct SeenAudioApp {
        std::wstring exe;      // normalized (lower case, ".exe")
        bool usedMic{false};   // had a capture (microphone) session
        std::int64_t lastActiveMs{0};  // GetTickCount64 of its last Active, 0 if never
    };
    std::vector<SeenAudioApp> seenAudioApps() const;

    // Called by nested COM objects — not part of the user API.
    void onTeamsSessionActive(DWORD pid, const std::wstring& exe, EDataFlow flow, ERole role, bool comms);
    void onTeamsSessionEnded(DWORD pid, const std::wstring& exe);
    void onNewSession(IAudioSessionControl* ctrl, EDataFlow flow, ERole role,
                      const std::wstring& deviceId);
    void onRenderSessionActive(const AudioSessionRef& session);
    void setCallMicActive(const void* session, bool active, const std::wstring& exe = {});
    void noteSeen(const std::wstring& exe, bool capture, bool active);

private:
    class Notification;          // IAudioSessionNotification — watches for new sessions
    class SessionEvents;         // IAudioSessionEvents — watches state on a specific session
    class EndpointNotification;  // IMMNotificationClient — watches for default-device / state changes

    struct SessionSub {
        Microsoft::WRL::ComPtr<IAudioSessionControl> ctrl;
        Microsoft::WRL::ComPtr<SessionEvents> events;
    };

    struct EndpointSub {
        std::wstring deviceId;
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        Microsoft::WRL::ComPtr<IAudioSessionManager2> mgr;
        Microsoft::WRL::ComPtr<Notification> notification;
        std::vector<SessionSub> sessions;
        EDataFlow flow;
        ERole role;
    };

    void staThreadProc(std::promise<bool>& readySignal);
    bool subscribeToEndpoint(EDataFlow flow, ERole role);
    // isNew: created after we subscribed (reported if already Active).
    void watchSession(IAudioSessionControl* ctrl, EDataFlow flow, ERole role,
                      const std::wstring& deviceId, bool isNew);
    void unsubscribeEndpoint(EndpointSub& ep);
    void unsubscribeAll();
    // Called on the STA thread (driven by WM_APP messages posted from the
    // IMMNotificationClient callbacks). Drops subs whose device is no longer
    // a default for any watched role, and re-subscribes to the current
    // defaults — picking up newly arrived endpoints (e.g. AirPods reconnect)
    // and dropping stale ones (e.g. AirPods unplugged).
    void rebuildSubscriptions();
    bool shouldReport(DWORD pid);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<EndpointNotification> m_endpointNotif;
    std::vector<EndpointSub> m_endpointSubs;

    std::thread m_staThread;
    DWORD m_staThreadId{0};
    std::atomic<bool> m_started{false};

    TriggerCallback       m_onTriggered;
    SessionEndedCallback  m_onSessionEnded;
    RenderSessionActiveCallback m_onRenderSessionActive;
    CallMicCallback       m_onCallMicChanged;

    TriggerAppList m_apps;

    std::mutex m_callMicMutex;
    std::vector<const void*> m_callMicSessions;  // call-app capture sessions now Active

    mutable std::mutex m_seenMutex;
    std::vector<SeenAudioApp> m_seen;

    std::mutex m_dedupeMutex;
    DWORD m_lastReportedPid{0};
    std::chrono::steady_clock::time_point m_lastReportedAt{};
    // Dedup for session-ended: suppress repeated Expired events (render + capture
    // both fire) within the same 20 s window used by shouldReport.
    DWORD m_lastEndedPid{0};
    std::chrono::steady_clock::time_point m_lastEndedAt{};
};

}
