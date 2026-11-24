#include "SessionMute.hpp"

#include "Logger.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Propods {

namespace {

using Microsoft::WRL::ComPtr;

// Identifies our own mute/unmute calls in session notifications.
// (Different from PlaybackSilencer's, so the two can be told apart.)
constexpr GUID kEventContext = {0x7c4a91e3, 0x2b58, 0x4d6f, {0xb1, 0x0e, 0x35, 0xa8, 0x9d, 0x62, 0xf7, 0x4c}};

// COM apartment guard: MTA, balanced only when CoInitializeEx says we must
// (S_OK / S_FALSE). RPC_E_CHANGED_MODE means the thread already has another
// apartment: proceed without touching it.
class ComScope {
public:
    ComScope() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (m_hr == S_OK || m_hr == S_FALSE) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
private:
    HRESULT m_hr;
};

std::wstring processPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) out.assign(path, len);
    CloseHandle(h);
    return out;
}

std::wstring baseName(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

std::filesystem::path heldMutedAppsFile() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::filesystem::path dir = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path{buf} / L"Propods" : std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"hold-muted-apps.txt";
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring instanceIdOf(IAudioSessionControl2* c) {
    LPWSTR raw = nullptr;
    if (FAILED(c->GetSessionInstanceIdentifier(&raw)) || !raw) return {};
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

// Calls fn(endpointId, sessionControl2) for every session on one device.
template <class Fn>
void forEachSessionOnDevice(IMMDevice* device, const std::wstring& endpointId, Fn&& fn) {
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(manager.GetAddressOf())))) return;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions))) return;
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, &control))) continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2))) continue;
        fn(endpointId, control2.Get());
    }
}

std::wstring deviceId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return {};
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

// Calls fn(endpointId, sessionControl2) for every session on every active output.
template <class Fn>
void forEachSessionOnAllOutputs(Fn&& fn) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return;
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT d = 0; d < deviceCount; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(d, &device))) continue;
        const std::wstring id = deviceId(device.Get());
        if (id.empty()) continue;
        forEachSessionOnDevice(device.Get(), id, fn);
    }
}

// Calls fn(endpointId, sessionControl2) for every session on the default
// render endpoint (eMultimedia).
template <class Fn>
void forEachSessionOnDefaultOutput(Fn&& fn) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDevice> device;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) return;
    const std::wstring id = deviceId(device.Get());
    if (id.empty()) return;
    forEachSessionOnDevice(device.Get(), id, fn);
}

// Calls fn(sessionControl2) for the sessions of one specific endpoint.
template <class Fn>
void forEachSessionOnEndpoint(const std::wstring& endpointId, Fn&& fn) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDevice> device;
    if (FAILED(en->GetDevice(endpointId.c_str(), &device))) return;
    forEachSessionOnDevice(device.Get(), endpointId,
        [&](const std::wstring&, IAudioSessionControl2* c) { fn(c); });
}

}  // namespace

SessionMute::SessionMute() = default;

void SessionMute::saveLocked() {
    std::vector<std::wstring> paths;
    for (const auto& m : m_muted)
        if (!m.path.empty() && std::find(paths.begin(), paths.end(), m.path) == paths.end()) paths.push_back(m.path);
    try {
        std::error_code ec;
        if (paths.empty()) { std::filesystem::remove(heldMutedAppsFile(), ec); return; }
        // UTF-8, one path per line (a wide stream in the "C" locale would drop
        // non-ASCII characters from the path).
        std::ofstream out{heldMutedAppsFile(), std::ios::trunc | std::ios::binary};
        for (const auto& p : paths) out << log::toUtf8(p) << '\n';
    } catch (...) {
        log::warn("SessionMute: could not update the recovery file");
    }
}

bool SessionMute::mute(const AudioSessionRef& session, const char* reason) {
    try {
        ComScope com;
        bool muted = false;
        bool found = false;
        forEachSessionOnEndpoint(session.endpointId, [&](IAudioSessionControl2* c) {
            if (found) return;
            if (session.instanceId.empty() || instanceIdOf(c) != session.instanceId) return;
            found = true;

            ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(c->QueryInterface(IID_PPV_ARGS(&volume)))) return;
            BOOL already = FALSE;
            if (FAILED(volume->GetMute(&already)) || already) return;  // the user's mute, or ours

            DWORD pid = 0;
            c->GetProcessId(&pid);
            if (!pid) pid = session.pid;
            const std::wstring path = pid ? processPath(pid) : std::wstring{};
            const std::wstring exe = !session.exe.empty() ? session.exe : baseName(path);

            // Record first, persist, then mute: a crash in between leaves a
            // record for a session that was never muted (harmless), never a
            // muted session without a record.
            {
                std::scoped_lock lk{m_mutex};
                const auto dup = std::find_if(m_muted.begin(), m_muted.end(), [&](const Muted& m) {
                    return m.endpointId == session.endpointId && m.instanceId == session.instanceId;
                });
                if (dup != m_muted.end()) return;
                m_muted.push_back({session.endpointId, session.instanceId, exe, path});
                saveLocked();
            }
            if (FAILED(volume->SetMute(TRUE, &kEventContext))) {
                std::scoped_lock lk{m_mutex};
                const auto it = std::find_if(m_muted.begin(), m_muted.end(), [&](const Muted& m) {
                    return m.endpointId == session.endpointId && m.instanceId == session.instanceId;
                });
                if (it != m_muted.end()) m_muted.erase(it);
                saveLocked();
                return;
            }
            muted = true;
            log::handover("HOLD    mute '{}' ({})", log::toUtf8(exe), reason);
        });
        return muted;
    } catch (...) {
        log::warn("SessionMute: mute failed unexpectedly");
        return false;
    }
}

int SessionMute::muteActiveSessionsOf(const std::function<bool(const std::wstring& exe)>& matchExe,
                                      const char* reason) {
    try {
        ComScope com;
        std::vector<AudioSessionRef> candidates;
        const DWORD self = GetCurrentProcessId();
        forEachSessionOnDefaultOutput([&](const std::wstring& endpointId, IAudioSessionControl2* c) {
            if (c->IsSystemSoundsSession() == S_OK) return;
            AudioSessionState state{};
            if (FAILED(c->GetState(&state)) || state != AudioSessionStateActive) return;
            DWORD pid = 0;
            c->GetProcessId(&pid);
            if (pid == self || pid == 0) return;
            const std::wstring exe = baseName(processPath(pid));
            if (exe.empty()) return;
            if (matchExe && !matchExe(exe)) return;
            ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(c->QueryInterface(IID_PPV_ARGS(&volume)))) return;
            BOOL already = FALSE;
            if (FAILED(volume->GetMute(&already)) || already) return;
            AudioSessionRef ref;
            ref.endpointId = endpointId;
            ref.instanceId = instanceIdOf(c);
            ref.pid = pid;
            ref.exe = exe;
            if (ref.instanceId.empty()) return;
            candidates.push_back(std::move(ref));
        });
        // mute() re-checks the mute state, records, persists, then mutes.
        int count = 0;
        for (const auto& ref : candidates)
            if (mute(ref, reason)) ++count;
        return count;
    } catch (...) {
        log::warn("SessionMute: muteActiveSessionsOf failed unexpectedly");
        return 0;
    }
}

int SessionMute::restoreAll(const char* reason) {
    std::vector<Muted> toRestore;
    {
        std::scoped_lock lk{m_mutex};
        toRestore.swap(m_muted);
        saveLocked();  // nothing left: deletes the file
    }
    if (toRestore.empty()) return 0;
    try {
        ComScope com;
        int restored = 0;
        forEachSessionOnAllOutputs([&](const std::wstring& endpointId, IAudioSessionControl2* c) {
            const std::wstring id = instanceIdOf(c);
            DWORD pid = 0;
            c->GetProcessId(&pid);
            const std::wstring path = pid ? processPath(pid) : std::wstring{};
            const auto it = std::find_if(toRestore.begin(), toRestore.end(), [&](const Muted& m) {
                // Sessions muted in this run are matched exactly. Only leftovers from
                // an earlier run (no instance id) match by executable path, so another
                // session of the same app that the user muted is never touched.
                if (!m.instanceId.empty()) return m.endpointId == endpointId && m.instanceId == id;
                return !m.path.empty() && !path.empty() && _wcsicmp(m.path.c_str(), path.c_str()) == 0;
            });
            if (it == toRestore.end()) return;
            ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(c->QueryInterface(IID_PPV_ARGS(&volume)))) return;
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMute(&muted)) && muted &&
                SUCCEEDED(volume->SetMute(FALSE, &kEventContext))) {
                ++restored;
                log::handover("HOLD    unmute '{}' ({})", log::toUtf8(it->exe), reason);
            }
        });
        return restored;
    } catch (...) {
        log::warn("SessionMute: restoreAll failed unexpectedly");
        return 0;
    }
}

bool SessionMute::anyMuted() {
    std::scoped_lock lk{m_mutex};
    return !m_muted.empty();
}

int SessionMute::restoreLeftovers() {
    std::vector<std::wstring> paths;
    try {
        std::ifstream in{heldMutedAppsFile(), std::ios::binary};
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::wstring p = fromUtf8(line);
            if (!p.empty() && std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(std::move(p));
        }
    } catch (...) {
        return 0;
    }
    if (paths.empty()) return 0;
    {
        std::scoped_lock lk{m_mutex};
        for (const auto& p : paths) m_muted.push_back({L"", L"", baseName(p), p});
    }
    const int n = restoreAll("left muted by an earlier run");
    log::info("SessionMute: restored {} session(s) left muted by an earlier run", n);
    return n;
}

}
