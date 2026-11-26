#include "TeamsCallWatcher.hpp"

#include "Logger.hpp"

#include <shlwapi.h>

#include <algorithm>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Propods {

using Microsoft::WRL::ComPtr;

namespace {
// Thread message posted by IMMNotificationClient callbacks to ask the STA
// thread to rebuild endpoint subscriptions. Handled in the message loop.
constexpr UINT WM_PROPODS_REBUILD = WM_APP + 1;
// Thread message carrying a session created on a WASAPI callback thread; the
// STA thread registers it (m_endpointSubs is only touched there, and
// registering inside OnSessionCreated itself is not allowed).
constexpr UINT WM_PROPODS_NEW_SESSION = WM_APP + 2;
struct NewSessionMsg {
    IAudioSessionControl* ctrl;  // AddRef'd by the poster, Released by the STA thread
    EDataFlow flow;
    ERole role;
    std::wstring deviceId;
};

// Roles we keep subscriptions for. eRender so we hear meeting/call audio
// starting, eCapture so we hear ringtones flowing into the mic side.
// Multimedia covers media-app triggers (VLC etc.).
struct WatchedRole { EDataFlow flow; ERole role; };
constexpr WatchedRole kWatchedRoles[] = {
    {eRender,  eCommunications},
    {eCapture, eCommunications},
    {eRender,  eMultimedia},
    {eCapture, eMultimedia},
};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

const char* flowLabel(EDataFlow flow) { return flow == eRender ? "render" : "capture"; }
const char* roleLabel(ERole role) {
    switch (role) {
        case eCommunications: return "comms";
        case eMultimedia:     return "multimedia";
        default:              return "console";
    }
}

std::wstring processBasename(DWORD pid) {
    if (!pid) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH]; DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &len))
        name = PathFindFileNameW(buf);
    CloseHandle(h);
    return name;
}

std::string narrow(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}


} // namespace

// ---------------------------------------------------------------------------
// SessionEvents — IAudioSessionEvents for one audio session
// Trigger apps (TriggerApps.hpp, looked up at each event so tray changes apply
// at once): fires onTeamsSessionActive / onTeamsSessionEnded and tracks open
// call mics. Any app on a render endpoint: fires onRenderSessionActive on an
// Active transition.
// ---------------------------------------------------------------------------

class TeamsCallWatcher::SessionEvents : public IAudioSessionEvents {
public:
    SessionEvents(TeamsCallWatcher* owner, std::wstring exe, DWORD pid,
                  EDataFlow flow, ERole role,
                  std::wstring endpointId, std::wstring instanceId)
        : m_owner(owner), m_exe(std::move(exe)), m_pid(pid),
          m_flow(flow), m_role(role),
          m_endpointId(std::move(endpointId)), m_instanceId(std::move(instanceId)) {}

    void clearOwner() {
        if (TeamsCallWatcher* owner = m_owner) owner->setCallMicActive(this, false);
        m_owner = nullptr;
    }
    const std::wstring& instanceId() const { return m_instanceId; }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionEvents)) {
            *out = static_cast<IAudioSessionEvents*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState state) override {
        TeamsCallWatcher* owner = m_owner;
        if (!owner) return S_OK;
        const bool capture = m_flow == eCapture;
        const std::optional<TriggerApp> app = owner->triggerApps().find(m_exe);
        // Always report a non-active state: an app removed from the list while
        // its mic was open must not count as a call forever.
        owner->setCallMicActive(this, app && isCallMic(*app, capture) && state == AudioSessionStateActive, m_exe);
        if (state == AudioSessionStateActive) {
            if (m_pid != GetCurrentProcessId()) owner->noteSeen(m_exe, capture, /*active=*/true);
            const TriggerEvent ev = app ? triggerFor(*app, capture) : TriggerEvent::None;
            if (ev != TriggerEvent::None)
                owner->onTeamsSessionActive(m_pid, m_exe, m_flow, m_role, ev == TriggerEvent::Call);
            if (m_flow == eRender && m_pid != GetCurrentProcessId()) {
                AudioSessionRef ref;
                ref.endpointId = m_endpointId;
                ref.instanceId = m_instanceId;
                ref.pid = m_pid;
                ref.exe = m_exe;
                owner->onRenderSessionActive(ref);
            }
        } else if (state == AudioSessionStateExpired && app) {
            owner->onTeamsSessionEnded(m_pid, m_exe);
        }
        return S_OK;
    }

    // Unused IAudioSessionEvents stubs
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override { return S_OK; }

private:
    std::atomic<ULONG> m_refs{1};
    TeamsCallWatcher* m_owner;
    std::wstring m_exe;
    DWORD m_pid;
    EDataFlow m_flow;
    ERole m_role;
    std::wstring m_endpointId;
    std::wstring m_instanceId;
};

// ---------------------------------------------------------------------------
// Notification — IAudioSessionNotification for the endpoint manager
// Fires when a brand-new session is created (e.g. a fresh ringtone stream).
// ---------------------------------------------------------------------------

class TeamsCallWatcher::Notification : public IAudioSessionNotification {
public:
    Notification(TeamsCallWatcher* owner, EDataFlow flow, ERole role, std::wstring deviceId)
        : m_owner(owner), m_flow(flow), m_role(role), m_deviceId(std::move(deviceId)) {}

    void clearOwner() { m_owner = nullptr; }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionNotification)) {
            *out = static_cast<IAudioSessionNotification*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* ctrl) override {
        if (ctrl && m_owner)
            m_owner->onNewSession(ctrl, m_flow, m_role, m_deviceId);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    TeamsCallWatcher* m_owner;
    EDataFlow m_flow;
    ERole m_role;
    std::wstring m_deviceId;
};

// ---------------------------------------------------------------------------
// EndpointNotification — IMMNotificationClient on the device enumerator.
//
// Fires on background MMDevice threads when audio devices come/go or when
// the system default for a role changes (e.g. Jabra Connect hijacks comms,
// AirPods unplug, USB headset arrives). We marshal each event to the STA
// thread via PostThreadMessage so rebuildSubscriptions() can mutate
// m_endpointSubs and call WASAPI APIs safely.
// ---------------------------------------------------------------------------

class TeamsCallWatcher::EndpointNotification : public IMMNotificationClient {
public:
    explicit EndpointNotification(DWORD staThreadId) : m_staThreadId(staThreadId) {}

    // Stop posting to the STA thread (used during stop()).
    void detach() { m_staThreadId.store(0); }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*id*/, DWORD /*state*/) override {
        // Any device state change (active/unplugged/disabled/etc.) might
        // change what the comms default resolves to. Coalesce into a rebuild.
        postRebuild();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*id*/) override   { postRebuild(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*id*/) override { postRebuild(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow /*flow*/, ERole /*role*/, LPCWSTR /*id*/) override {
        // The most important event for us: third-party utilities (Jabra
        // Connect, Logitech, etc.) flipping the system default to their own
        // device while a different one is the user's intent.
        postRebuild();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*id*/, const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

private:
    void postRebuild() {
        DWORD tid = m_staThreadId.load();
        if (tid) PostThreadMessageW(tid, WM_PROPODS_REBUILD, 0, 0);
    }

    std::atomic<ULONG> m_refs{1};
    std::atomic<DWORD> m_staThreadId;
};

// ---------------------------------------------------------------------------
// TeamsCallWatcher implementation
// ---------------------------------------------------------------------------

void TeamsCallWatcher::onTeamsSessionActive(DWORD pid, const std::wstring& exe,
                                             EDataFlow flow, ERole role, bool comms) {
    if (!shouldReport(pid)) {
        log::debug("Watched app active (dedupe suppressed): {} pid {} on {}/{}",
                   narrow(exe), (unsigned)pid, flowLabel(flow), roleLabel(role));
        return;
    }
    log::info("Watched app {} active: {} (pid {}) on {}/{}",
              comms ? "call/meeting" : "media playback",
              narrow(exe), (unsigned)pid, flowLabel(flow), roleLabel(role));
    log::handover("TRIGGER {} active: {} (pid {}) on {}/{}",
                  comms ? "call/meeting" : "media",
                  narrow(exe), (unsigned)pid, flowLabel(flow), roleLabel(role));
    // Feed into the handover state machine — same effect as GSMTC media-start.
    if (m_onTriggered) m_onTriggered(comms);
}

void TeamsCallWatcher::onRenderSessionActive(const AudioSessionRef& session) {
    // Copy first: the callback is set once before start(), but never call a
    // member std::function in place from a WASAPI callback thread.
    RenderSessionActiveCallback cb = m_onRenderSessionActive;
    if (cb) cb(session);
}

void TeamsCallWatcher::setCallMicActive(const void* session, bool active, const std::wstring& exe) {
    bool edge = false, open = false;
    {
        std::scoped_lock lk{m_callMicMutex};
        auto it = std::find(m_callMicSessions.begin(), m_callMicSessions.end(), session);
        const bool wasOpen = !m_callMicSessions.empty();
        if (active && it == m_callMicSessions.end()) m_callMicSessions.push_back(session);
        if (!active && it != m_callMicSessions.end()) m_callMicSessions.erase(it);
        open = !m_callMicSessions.empty();
        edge = open != wasOpen;
    }
    if (!edge) return;
    log::info("TeamsCallWatcher: keep app mic {} ({})", open ? "open (call or meeting)" : "closed",
              exe.empty() ? std::string{"session gone"} : narrow(exe));
    CallMicCallback cb = m_onCallMicChanged;
    if (cb) cb(open);
}

void TeamsCallWatcher::noteSeen(const std::wstring& exe, bool capture, bool active) {
    const std::wstring n = normalizeExeName(exe);
    if (n.empty()) return;
    std::scoped_lock lk{m_seenMutex};
    auto it = std::find_if(m_seen.begin(), m_seen.end(), [&](const SeenAudioApp& a) { return a.exe == n; });
    if (it == m_seen.end()) {
        if (m_seen.size() >= 64) return;  // bounded; the first apps seen are the ones that matter
        m_seen.push_back({n, false, 0});
        it = m_seen.end() - 1;
    }
    if (capture) it->usedMic = true;
    if (active) it->lastActiveMs = static_cast<std::int64_t>(GetTickCount64());
}

std::vector<TeamsCallWatcher::SeenAudioApp> TeamsCallWatcher::seenAudioApps() const {
    std::vector<SeenAudioApp> out;
    {
        std::scoped_lock lk{m_seenMutex};
        out = m_seen;
    }
    std::stable_sort(out.begin(), out.end(), [](const SeenAudioApp& a, const SeenAudioApp& b) {
        return a.lastActiveMs > b.lastActiveMs;
    });
    return out;
}

void TeamsCallWatcher::onTeamsSessionEnded(DWORD pid, const std::wstring& exe) {
    // Dedup: render and capture sessions both fire Expired when a call ends.
    // Suppress duplicate notifications within a 20 s window.
    {
        using namespace std::chrono;
        std::scoped_lock lk{m_dedupeMutex};
        auto now = steady_clock::now();
        if (pid == m_lastEndedPid && now - m_lastEndedAt < seconds(20))
            return;
        m_lastEndedPid = pid;
        m_lastEndedAt  = now;
    }
    log::info("TeamsCallWatcher: session expired for {} (pid {}) — call ended",
              narrow(exe), (unsigned)pid);
    if (m_onSessionEnded) m_onSessionEnded();
}

void TeamsCallWatcher::onNewSession(IAudioSessionControl* ctrl,
                                    EDataFlow flow, ERole role,
                                    const std::wstring& deviceId) {
    // WASAPI callback thread: hand the session to the STA thread.
    const DWORD tid = m_staThreadId;
    if (!ctrl || !tid) return;
    ctrl->AddRef();
    auto* msg = new NewSessionMsg{ctrl, flow, role, deviceId};
    if (!PostThreadMessageW(tid, WM_PROPODS_NEW_SESSION, 0, reinterpret_cast<LPARAM>(msg))) {
        ctrl->Release();
        delete msg;
    }
}

void TeamsCallWatcher::watchSession(IAudioSessionControl* ctrl,
                                    EDataFlow flow, ERole role,
                                    const std::wstring& deviceId, bool isNew) {
    ComPtr<IAudioSessionControl2> ctrl2;
    if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&ctrl2)))) return;

    DWORD pid = 0;
    if (FAILED(ctrl2->GetProcessId(&pid)) || !pid) return;

    // System-sounds session — skip.
    if (ctrl2->IsSystemSoundsSession() == S_OK) return;

    std::wstring exe = processBasename(pid);
    if (exe.empty()) return;

    AudioSessionState state{};
    ctrl->GetState(&state);

    log::debug("TeamsCallWatcher: existing session {} (pid {}) on {}/{} state={}",
               narrow(exe), (unsigned)pid, flowLabel(flow), roleLabel(role), (int)state);

    // Every app is watched on every endpoint: whether it is a trigger app is
    // decided at each event, so an app added from the tray works at once.
    // Event driven only: no audio stream is opened and nothing is polled.
    const bool capture = flow == eCapture;
    const std::optional<TriggerApp> app = triggerApps().find(exe);
    if (pid != GetCurrentProcessId()) noteSeen(exe, capture, state == AudioSessionStateActive);

    // The EndpointSub this session belongs to: match by device id, not just
    // flow/role, so cleanup always unregisters what we registered.
    EndpointSub* sub = nullptr;
    for (auto& ep : m_endpointSubs) {
        if (ep.flow == flow && ep.deviceId == deviceId) { sub = &ep; break; }
    }
    if (!sub) return;

    // If a trigger app is already active right now (Teams was in a call before
    // we started), report. Other apps already playing at registration time are
    // deliberately not reported: only real transitions are.
    const TriggerEvent trigger = app ? triggerFor(*app, capture) : TriggerEvent::None;
    if (trigger != TriggerEvent::None && state == AudioSessionStateActive)
        onTeamsSessionActive(pid, exe, flow, role, trigger == TriggerEvent::Call);

    std::wstring instanceId;
    LPWSTR rawInstance = nullptr;
    if (SUCCEEDED(ctrl2->GetSessionInstanceIdentifier(&rawInstance)) && rawInstance) {
        instanceId = rawInstance;
        CoTaskMemFree(rawInstance);
    }

    // Register for future state changes.
    ComPtr<SessionEvents> ev;
    ev.Attach(new SessionEvents(this, exe, pid, flow, role, deviceId, std::move(instanceId)));
    const bool registered = SUCCEEDED(ctrl->RegisterAudioSessionNotification(ev.Get()));
    // A call already in progress when we (re)subscribed.
    if (registered && app && isCallMic(*app, capture) && state == AudioSessionStateActive)
        setCallMicActive(ev.Get(), true, exe);
    // A brand-new session can be Active before we got to register for its
    // events (it reached us via the STA queue): report that start now.
    if (isNew && state == AudioSessionStateActive && flow == eRender && pid != GetCurrentProcessId()) {
        AudioSessionRef ref;
        ref.endpointId = deviceId;
        ref.instanceId = ev->instanceId();
        ref.pid = pid;
        ref.exe = exe;
        onRenderSessionActive(ref);
    }
    if (registered)
        sub->sessions.push_back({ctrl, std::move(ev)});
}

bool TeamsCallWatcher::shouldReport(DWORD pid) {
    using namespace std::chrono;
    std::scoped_lock lk{m_dedupeMutex};
    auto now = steady_clock::now();
    if (pid == m_lastReportedPid && now - m_lastReportedAt < seconds(20))
        return false;
    m_lastReportedPid = pid;
    m_lastReportedAt = now;
    return true;
}

bool TeamsCallWatcher::subscribeToEndpoint(EDataFlow flow, ERole role) {
    ComPtr<IMMDevice> endpoint;
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(flow, role, &endpoint);
    if (FAILED(hr)) {
        log::debug("TeamsCallWatcher: GetDefaultAudioEndpoint({}/{}) failed: 0x{:08X}",
                   flowLabel(flow), roleLabel(role), (unsigned)hr);
        return false;
    }

    LPWSTR idRaw = nullptr;
    endpoint->GetId(&idRaw);
    std::wstring deviceId = idRaw ? idRaw : L"";
    if (idRaw) CoTaskMemFree(idRaw);

    // Don't double-register on the same physical device for the same flow direction.
    for (const auto& ep : m_endpointSubs) {
        if (ep.flow == flow && ep.deviceId == deviceId) {
            log::debug("TeamsCallWatcher: skipping duplicate device for {}/{} (same as {})",
                       flowLabel(flow), roleLabel(role), roleLabel(ep.role));
            return true;
        }
    }

    ComPtr<IAudioSessionManager2> mgr;
    hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr);
    if (FAILED(hr)) {
        log::warn("TeamsCallWatcher: Activate(IAudioSessionManager2) on {}/{} failed: 0x{:08X}",
                  flowLabel(flow), roleLabel(role), (unsigned)hr);
        return false;
    }

    // Register for new sessions.
    ComPtr<Notification> notif;
    notif.Attach(new Notification(this, flow, role, deviceId));
    hr = mgr->RegisterSessionNotification(notif.Get());
    if (FAILED(hr)) {
        log::warn("TeamsCallWatcher: RegisterSessionNotification on {}/{} failed: 0x{:08X}",
                  flowLabel(flow), roleLabel(role), (unsigned)hr);
        notif->clearOwner();
        return false;
    }

    log::info("TeamsCallWatcher: watching {}/{} (id={})",
              flowLabel(flow), roleLabel(role), narrow(deviceId));
    m_endpointSubs.push_back({std::move(deviceId), std::move(endpoint),
                               std::move(mgr), std::move(notif), {}, flow, role});

    // Enumerate and watch sessions that already exist on this endpoint.
    ComPtr<IAudioSessionEnumerator> sessEnum;
    if (SUCCEEDED(m_endpointSubs.back().mgr->GetSessionEnumerator(&sessEnum))) {
        int count = 0;
        sessEnum->GetCount(&count);
        log::debug("TeamsCallWatcher: {} existing session(s) on {}/{}", count,
                   flowLabel(flow), roleLabel(role));
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> ctrl;
            if (SUCCEEDED(sessEnum->GetSession(i, &ctrl)))
                watchSession(ctrl.Get(), flow, role, m_endpointSubs.back().deviceId, /*isNew=*/false);
        }
    }

    return true;
}

void TeamsCallWatcher::unsubscribeEndpoint(EndpointSub& ep) {
    for (auto& ss : ep.sessions) {
        if (ss.ctrl && ss.events) {
            ss.ctrl->UnregisterAudioSessionNotification(ss.events.Get());
            ss.events->clearOwner();
        }
    }
    ep.sessions.clear();
    if (ep.mgr && ep.notification) {
        ep.mgr->UnregisterSessionNotification(ep.notification.Get());
        ep.notification->clearOwner();
    }
}

void TeamsCallWatcher::unsubscribeAll() {
    for (auto& ep : m_endpointSubs) unsubscribeEndpoint(ep);
    m_endpointSubs.clear();
    if (m_endpointNotif && m_enumerator) {
        m_enumerator->UnregisterEndpointNotificationCallback(m_endpointNotif.Get());
        m_endpointNotif->detach();
        m_endpointNotif.Reset();
    }
    m_enumerator.Reset();
}

void TeamsCallWatcher::rebuildSubscriptions() {
    if (!m_enumerator) return;

    // Snapshot the current default device IDs for each (flow, role) we care about.
    std::set<std::wstring> desiredIds;
    for (auto wr : kWatchedRoles) {
        ComPtr<IMMDevice> dev;
        if (FAILED(m_enumerator->GetDefaultAudioEndpoint(wr.flow, wr.role, &dev)) || !dev)
            continue;
        LPWSTR idRaw = nullptr;
        if (FAILED(dev->GetId(&idRaw)) || !idRaw) continue;
        desiredIds.emplace(idRaw);
        CoTaskMemFree(idRaw);
    }

    // Drop subs whose device is no longer the default for any watched role.
    // (Includes endpoints that were unplugged or whose role default flipped
    // to a different physical device.)
    for (auto it = m_endpointSubs.begin(); it != m_endpointSubs.end();) {
        if (desiredIds.find(it->deviceId) == desiredIds.end()) {
            log::info("TeamsCallWatcher: dropping sub for {} (no longer a watched default)",
                      narrow(it->deviceId));
            unsubscribeEndpoint(*it);
            it = m_endpointSubs.erase(it);
        } else {
            ++it;
        }
    }

    // Re-subscribe to current defaults. subscribeToEndpoint() is a no-op for
    // (flow, deviceId) pairs we already track, so this is idempotent.
    for (auto wr : kWatchedRoles) subscribeToEndpoint(wr.flow, wr.role);
}

void TeamsCallWatcher::staThreadProc(std::promise<bool>& readySignal) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        log::error("TeamsCallWatcher STA: CoInitializeEx failed: 0x{:08X}", (unsigned)hr);
        readySignal.set_value(false);
        return;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) {
        log::error("TeamsCallWatcher: CoCreateInstance(MMDeviceEnumerator) failed: 0x{:08X}",
                   (unsigned)hr);
        CoUninitialize();
        readySignal.set_value(false);
        return;
    }

    for (auto wr : kWatchedRoles) subscribeToEndpoint(wr.flow, wr.role);

    if (m_endpointSubs.empty()) {
        log::error("TeamsCallWatcher: no endpoints registered");
        CoUninitialize();
        readySignal.set_value(false);
        return;
    }

    // Now that the initial subs are in place, hook the device-enumerator's
    // change notifications so the next plug/unplug/default-change triggers
    // a rebuild instead of leaving us listening to a ghost device.
    m_endpointNotif.Attach(new EndpointNotification(m_staThreadId));
    HRESULT regHr = m_enumerator->RegisterEndpointNotificationCallback(m_endpointNotif.Get());
    if (FAILED(regHr)) {
        log::warn("TeamsCallWatcher: RegisterEndpointNotificationCallback failed: 0x{:08X} - "
                  "device add/remove and default-change events will be missed",
                  (unsigned)regHr);
        m_endpointNotif->detach();
        m_endpointNotif.Reset();
    } else {
        log::info("TeamsCallWatcher: endpoint notifications armed - will rebuild on "
                  "default-device / state changes");
    }

    readySignal.set_value(true);

    // STA message loop — WASAPI delivers callbacks here. WM_PROPODS_REBUILD
    // is our own thread message posted from EndpointNotification callbacks.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_PROPODS_REBUILD) {
            rebuildSubscriptions();
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_PROPODS_NEW_SESSION) {
            auto* m = reinterpret_cast<NewSessionMsg*>(msg.lParam);
            watchSession(m->ctrl, m->flow, m->role, m->deviceId, /*isNew=*/true);
            m->ctrl->Release();
            delete m;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Drop sessions still queued for registration (release their refs).
    MSG pending;
    while (PeekMessageW(&pending, nullptr, WM_PROPODS_NEW_SESSION, WM_PROPODS_NEW_SESSION, PM_REMOVE)) {
        auto* m = reinterpret_cast<NewSessionMsg*>(pending.lParam);
        if (m) { m->ctrl->Release(); delete m; }
    }
    unsubscribeAll();
    CoUninitialize();
}

TeamsCallWatcher::TeamsCallWatcher() = default;
TeamsCallWatcher::~TeamsCallWatcher() { stop(); }

bool TeamsCallWatcher::start() {
    if (m_started.exchange(true)) return true;

    std::promise<bool> readySignal;
    auto ready = readySignal.get_future();

    m_staThread = std::thread([this, &readySignal]() {
        m_staThreadId = GetCurrentThreadId();
        staThreadProc(readySignal);
    });

    const bool ok = ready.get();
    if (!ok) { m_staThread.join(); m_started.store(false); }
    return ok;
}

void TeamsCallWatcher::stop() {
    if (!m_started.exchange(false)) return;
    if (m_staThreadId)
        PostThreadMessageW(m_staThreadId, WM_QUIT, 0, 0);
    if (m_staThread.joinable())
        m_staThread.join();
    m_staThreadId = 0;
    log::info("TeamsCallWatcher: stopped");
}

}
