#include "PlaybackSilencer.hpp"

#include "Logger.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace Propods {

namespace {

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Media::Control;

// Identifies our own mute/unmute calls in session notifications.
constexpr GUID kEventContext = {0x5b1e3c2a, 0x7d41, 0x4f0e, {0x9a, 0x3b, 0x61, 0x2c, 0x8e, 0x0d, 0x44, 0x17}};
constexpr auto kSettleBeforeMute = std::chrono::milliseconds{1500};
constexpr float kAudiblePeak = 0.001f;  // about -60 dBFS

std::wstring processName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"pid " + std::to_wstring(pid);
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    std::wstring name = L"pid " + std::to_wstring(pid);
    if (QueryFullProcessImageNameW(h, 0, path, &len)) name = std::filesystem::path(path).filename().wstring();
    CloseHandle(h);
    return name;
}

// Calls fn(endpointId, sessionControl2) for every session on every active output.
template <class Fn>
void forEachSession(Fn&& fn) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return;
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT d = 0; d < deviceCount; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(d, &device))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;
        const std::wstring endpointId = rawId;
        CoTaskMemFree(rawId);
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf())))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
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
}

std::wstring instanceId(IAudioSessionControl2* c) {
    LPWSTR raw = nullptr;
    if (FAILED(c->GetSessionInstanceIdentifier(&raw)) || !raw) return {};
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

void addName(std::vector<std::wstring>& names, const std::wstring& name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
}

}  // namespace

void PlaybackSilencer::silenceAsync(const char* reason) {
    if (!m_enabled.load()) {
        log::info("Silencer: disabled; leaving PC audio as is ({})", reason);
        return;
    }
    if (m_running.exchange(true)) return;
    std::string why = reason;
    std::thread([this, why]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Result r;
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds{800});
            if (m_stillReleased && !m_stillReleased()) {
                log::handover("SILENCE skipped: the AirPods came back within 0.8 s ({})", why);
                m_running.store(false);
                return;
            }
            r = silence(why.c_str());
        } catch (const winrt::hresult_error& e) {
            log::warn("Silencer: failed 0x{:08X} {}", static_cast<std::uint32_t>(e.code().value),
                winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            log::warn("Silencer: failed {}", e.what());
        }
        m_running.store(false);
        if (m_onDone && (r.paused > 0 || r.muted > 0)) m_onDone(r);
    }).detach();
}

PlaybackSilencer::Result PlaybackSilencer::silence(const char* reason) {
    Result r;
    log::handover("SILENCE {}: pausing playing media sessions", reason);
    r.paused = pausePlayingSessions(r.names);
    std::this_thread::sleep_for(kSettleBeforeMute);
    r.muted = muteAudibleSessions(r.names);
    log::handover("SILENCE done: {} paused, {} muted", r.paused, r.muted);
    return r;
}

int PlaybackSilencer::pausePlayingSessions(std::vector<std::wstring>& names) {
    int paused = 0;
    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    auto sessions = manager.GetSessions();
    for (const auto& session : sessions) {
        std::wstring app;
        try { app = std::wstring(session.SourceAppUserModelId()); } catch (...) {}
        GlobalSystemMediaTransportControlsSessionPlaybackStatus status{};
        try { status = session.GetPlaybackInfo().PlaybackStatus(); } catch (...) { continue; }
        if (status != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) continue;
        bool ok = false;
        try { ok = session.TryPauseAsync().get(); } catch (...) {}
        log::handover("SILENCE pause '{}': {}", log::toUtf8(app), ok ? "paused" : "refused");
        if (ok) {
            ++paused;
            addName(names, app);
        }
    }
    return paused;
}

int PlaybackSilencer::muteAudibleSessions(std::vector<std::wstring>& names) {
    struct Candidate {
        std::wstring endpointId, instance, name;
        ComPtr<IAudioMeterInformation> meter;
        ComPtr<ISimpleAudioVolume> volume;
        float peak = 0.0f;
    };
    std::vector<Candidate> candidates;
    const DWORD self = GetCurrentProcessId();
    forEachSession([&](const std::wstring& endpointId, IAudioSessionControl2* c) {
        if (c->IsSystemSoundsSession() == S_OK) return;
        AudioSessionState state{};
        if (FAILED(c->GetState(&state)) || state != AudioSessionStateActive) return;
        DWORD pid = 0;
        c->GetProcessId(&pid);
        if (pid == self || pid == 0) return;
        Candidate cand;
        if (FAILED(c->QueryInterface(IID_PPV_ARGS(&cand.meter))) ||
            FAILED(c->QueryInterface(IID_PPV_ARGS(&cand.volume)))) return;
        BOOL muted = FALSE;
        if (FAILED(cand.volume->GetMute(&muted)) || muted) return;
        cand.endpointId = endpointId;
        cand.instance = instanceId(c);
        cand.name = processName(pid);
        candidates.push_back(std::move(cand));
    });

    // A paused player can keep its stream open; only mute what is audible.
    for (int sample = 0; sample < 4; ++sample) {
        for (auto& cand : candidates) {
            float peak = 0.0f;
            if (SUCCEEDED(cand.meter->GetPeakValue(&peak))) cand.peak = std::max(cand.peak, peak);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    int muted = 0;
    for (auto& cand : candidates) {
        if (cand.peak < kAudiblePeak) continue;
        if (FAILED(cand.volume->SetMute(TRUE, &kEventContext))) continue;
        ++muted;
        addName(names, cand.name);
        log::handover("SILENCE mute '{}' (peak {:.3f})", log::toUtf8(cand.name), cand.peak);
        std::scoped_lock lk{m_mutex};
        m_muted.push_back({cand.endpointId, cand.instance, cand.name});
    }
    return muted;
}

bool PlaybackSilencer::hasMuted() {
    std::scoped_lock lk{m_mutex};
    return !m_muted.empty();
}

int PlaybackSilencer::restoreMutes(const char* reason) {
    std::vector<MutedSession> toRestore;
    {
        std::scoped_lock lk{m_mutex};
        toRestore.swap(m_muted);
    }
    if (toRestore.empty()) return 0;
    int restored = 0;
    forEachSession([&](const std::wstring& endpointId, IAudioSessionControl2* c) {
        const std::wstring id = instanceId(c);
        const auto it = std::find_if(toRestore.begin(), toRestore.end(), [&](const MutedSession& m) {
            return m.endpointId == endpointId && m.instanceId == id;
        });
        if (it == toRestore.end()) return;
        ComPtr<ISimpleAudioVolume> volume;
        if (FAILED(c->QueryInterface(IID_PPV_ARGS(&volume)))) return;
        BOOL muted = FALSE;
        if (SUCCEEDED(volume->GetMute(&muted)) && muted &&
            SUCCEEDED(volume->SetMute(FALSE, &kEventContext))) {
            ++restored;
            log::handover("SILENCE unmute '{}' ({})", log::toUtf8(it->name), reason);
        }
    });
    return restored;
}

}
