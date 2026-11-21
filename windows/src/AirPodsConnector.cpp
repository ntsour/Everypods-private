#include "AirPodsConnector.hpp"

#include "Logger.hpp"

#include <windows.h>
#include <winioctl.h>  // IOCTL_KS_PROPERTY (excluded by WIN32_LEAN_AND_MEAN)
#include <bluetoothapis.h>
#pragma comment(lib, "Bthprops.lib")

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Networking.h>

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

// Undocumented IPolicyConfigVista interface for setting the default audio endpoint.
// Used by AudioSwitcher, SoundSwitch, NirCmd, and many open-source tools.
namespace {

const CLSID CLSID_CPolicyConfigVistaClient = {
    0x294935CEu, 0xF637, 0x4E7C,
    { 0xA4, 0x1B, 0xAB, 0x25, 0x54, 0x60, 0xB8, 0x62 }
};

struct DECLSPEC_UUID("568b9108-44bf-40b4-9006-86afe5b5a620")
IPolicyConfigVista : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

}

namespace Propods {

namespace {

// A2DP Sink profile: 0000110B-0000-1000-8000-00805F9B34FB
static const GUID kA2dpSink = {
    0x0000110B, 0x0000, 0x1000,
    { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB }
};

HANDLE openFirstRadio() {
    BLUETOOTH_FIND_RADIO_PARAMS p = { sizeof(p) };
    HANDLE hRadio = nullptr;
    HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&p, &hRadio);
    if (hFind) BluetoothFindRadioClose(hFind);
    return hRadio;
}

}

AirPodsConnector::AirPodsConnector(std::uint64_t address) : m_address(address) {}

AirPodsConnector::~AirPodsConnector() {
}

namespace {

// HandsFree profile: 0000111E-0000-1000-8000-00805F9B34FB
static const GUID kHandsFree = {
    0x0000111E, 0x0000, 0x1000,
    { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB }
};

// Dump everything Windows knows about the BT device + the GUID list of services
// it thinks the device advertises. Logged before/after each BluetoothSetServiceState
// call so that when it returns 87 (ERROR_INVALID_PARAMETER) we can tell at a glance
// whether the issue is pairing state, driver, or the kA2dpSink GUID simply not being
// in the installed-services list (which would explain error 87 — Windows believes
// the device doesn't expose A2DP).
void logDeviceInfo(std::uint64_t address, const char* tag) {
    HANDLE hRadio = openFirstRadio();
    if (!hRadio) {
        log::warn("logDeviceInfo[{}]: no Bluetooth radio", tag);
        return;
    }

    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof(info);
    info.Address.ullLong = address;

    DWORD getErr = BluetoothGetDeviceInfo(hRadio, &info);
    if (getErr != ERROR_SUCCESS) {
        log::warn("logDeviceInfo[{}]: BluetoothGetDeviceInfo({:012X}) failed: {} (0x{:08X})",
            tag, address, getErr, getErr);
        CloseHandle(hRadio);
        return;
    }
    log::info("  [BT-diag {}] device {:012X}: connected={} authenticated={} remembered={} class=0x{:08X}",
        tag,
        address,
        info.fConnected ? "yes" : "no",
        info.fAuthenticated ? "yes" : "no",
        info.fRemembered ? "yes" : "no",
        info.ulClassofDevice);

    // Probe for service count, then fetch the list.
    DWORD numServices = 0;
    DWORD enumErr = BluetoothEnumerateInstalledServices(hRadio, &info, &numServices, nullptr);
    if (enumErr != ERROR_SUCCESS && enumErr != ERROR_MORE_DATA) {
        log::warn("  [BT-diag {}] BluetoothEnumerateInstalledServices(probe) failed: {} (0x{:08X})",
            tag, enumErr, enumErr);
        CloseHandle(hRadio);
        return;
    }

    bool a2dpAdvertised = false;
    bool hfpAdvertised = false;
    if (numServices > 0) {
        std::vector<GUID> services(numServices);
        enumErr = BluetoothEnumerateInstalledServices(hRadio, &info, &numServices, services.data());
        if (enumErr == ERROR_SUCCESS) {
            log::info("  [BT-diag {}] installed services on {:012X}: {} entries",
                tag, address, numServices);
            for (DWORD i = 0; i < numServices; ++i) {
                const GUID& g = services[i];
                wchar_t guidStr[64] = {};
                StringFromGUID2(g, guidStr, 64);
                bool isA2dp = IsEqualGUID(g, kA2dpSink) != FALSE;
                bool isHfp  = IsEqualGUID(g, kHandsFree) != FALSE;
                a2dpAdvertised = a2dpAdvertised || isA2dp;
                hfpAdvertised  = hfpAdvertised  || isHfp;
                log::debug("    service[{}] = {} {}",
                    i,
                    winrt::to_string(guidStr),
                    isA2dp ? "[A2DP_SINK]" : isHfp ? "[HandsFree]" : "");
            }
        } else {
            log::warn("  [BT-diag {}] BluetoothEnumerateInstalledServices(payload) failed: {} (0x{:08X})",
                tag, enumErr, enumErr);
        }
    }
    log::info("  [BT-diag {}] A2DP advertised: {} | HFP advertised: {}",
        tag,
        a2dpAdvertised ? "yes" : "NO",
        hfpAdvertised  ? "yes" : "NO");

    CloseHandle(hRadio);

    // Cross-reference with the WinRT view.
    try {
        using namespace winrt::Windows::Devices::Bluetooth;
        auto device = BluetoothDevice::FromBluetoothAddressAsync(address).get();
        if (device) {
            log::info("  [BT-diag {}] WinRT BluetoothDevice.ConnectionStatus = {}",
                tag, (int)device.ConnectionStatus());
        } else {
            log::info("  [BT-diag {}] WinRT BluetoothDevice = null", tag);
        }
    } catch (const winrt::hresult_error& e) {
        log::debug("  [BT-diag {}] WinRT lookup failed: 0x{:08X}",
            tag, (std::uint32_t)e.code().value);
    }
}

// `quiet=true` suppresses the success log line and the [BT-diag] dump on failure —
// used by the periodic A2DP-nudge thread that runs after connect() to repeatedly
// poke the BT stack until the audio endpoint reaches ACTIVE. Without quiet=true,
// the log floods with diagnostics every 3 seconds during the negotiation wait.
bool toggleProfile(std::uint64_t address, const GUID& profile, DWORD state,
                   const char* label, bool quiet = false) {
    HANDLE hRadio = openFirstRadio();
    if (!hRadio) {
        if (!quiet) log::error("No Bluetooth radio found");
        return false;
    }

    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof(info);
    info.Address.ullLong = address;

    // Populate class-of-device and other fields from the system's paired-device record.
    DWORD getErr = BluetoothGetDeviceInfo(hRadio, &info);
    if (getErr != ERROR_SUCCESS) {
        if (!quiet) {
            log::warn("BluetoothGetDeviceInfo({:012X}) returned {} (0x{:08X}) — device may not be paired",
                address, getErr, getErr);
        }
        CloseHandle(hRadio);
        return false;
    }
    if (!quiet) {
        log::debug("  device: connected={} class=0x{:08X}",
            info.fConnected ? "yes" : "no",
            info.ulClassofDevice);
    }

    DWORD setErr = BluetoothSetServiceState(hRadio, &info, &profile, state);
    CloseHandle(hRadio);

    if (setErr != ERROR_SUCCESS) {
        if (!quiet) {
            log::warn("BluetoothSetServiceState({}, {}) returned {} (0x{:08X})",
                label, state == BLUETOOTH_SERVICE_ENABLE ? "ENABLE" : "DISABLE",
                setErr, setErr);
            // The call just failed — dump everything Windows knows about the device.
            // If the kA2dpSink GUID is not in the installed-services list below, then
            // error 87 = ERROR_INVALID_PARAMETER is "explained" (Windows believes the
            // device doesn't advertise this service) and the next step is to inspect
            // the SDP records / re-pair the device.
            std::string diagTag = std::string(label) + " post-fail";
            logDeviceInfo(address, diagTag.c_str());
        }
        return false;
    }
    return true;
}

}

namespace {
// Defined further down, next to the endpoint enumeration it uses.
constexpr ULONG kKsOneshotReconnect = 0;  // KSPROPERTY_ONESHOT_RECONNECT
constexpr ULONG kKsOneshotDisconnect = 1; // KSPROPERTY_ONESHOT_DISCONNECT
int sendBtAudioOneshot(ULONG propertyId, const char* label);
}

bool AirPodsConnector::connect() {
    if (m_address == 0) {
        log::warn("AirPods address not configured; use tray menu \"Select AirPods...\"");
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    log::debug("AirPodsConnector::connect() address={:012X}", m_address);

    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
    using namespace winrt::Windows::Networking::Sockets;

    BluetoothDevice device = nullptr;
    try {
        device = BluetoothDevice::FromBluetoothAddressAsync(m_address).get();
        if (!device) {
            log::warn("AirPods FromBluetoothAddressAsync returned null");
            return false;
        }
        log::debug("  status before: connection={}", (int)device.ConnectionStatus());
    } catch (const winrt::hresult_error& e) {
        log::warn("FromBluetoothAddressAsync failed: 0x{:08X} {}",
            (std::uint32_t)e.code().value, winrt::to_string(e.message()));
        return false;
    }

    // Order matters here. AirPods are typically paired but disconnected at the
    // ACL layer when the user puts them back in their ears. BluetoothSetService-
    // State CANNOT bring up the classical-BT link; it only toggles profile state
    // on an already-connected device. So:
    //
    //   1. FIRST: uncached RFCOMM enumeration. This forces a fresh L2CAP+SDP
    //      exchange (10-15s) which side-effect-establishes the classical-BT link.
    //      Opening a socket on a returned service is best-effort: some drivers
    //      need the socket to fully wake up; others activate on SDP alone.
    //
    //   2. THEN: BluetoothSetServiceState(A2DP/HFP, ENABLE) — the documented
    //      profile-activation path. Now that the link is up (per step 1), this
    //      call has a real chance of succeeding rather than returning error 87
    //      (ERROR_INVALID_PARAMETER, which in practice means "device not
    //      currently connected at the BT layer"). Logs full BT diagnostics if
    //      it still fails — see logDeviceInfo in toggleProfile.
    //
    // After this returns, the IMMNotificationClient is responsible for finishing
    // the routing when (if) the audio endpoint transitions to DEVICE_STATE_ACTIVE.
    // This function no longer waits for that — it just kicks the OS.

    // Preferred path: ask the Bluetooth audio driver itself to connect, the same
    // request Settings -> Bluetooth -> Connect makes (KSPROPERTY_ONESHOT_RECONNECT
    // on the AirPods' A2DP and hands-free filters). On the test PC
    // (ks_connect_spike, 2026-11-20) this brought audio ACTIVE in ~5 s median
    // versus ~16 s for the SDP/RFCOMM wake below, and the link then stayed up
    // through 5 idle minutes. The SDP/RFCOMM wake remains as a fallback for
    // drivers that do not expose the property.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const int sent = sendBtAudioOneshot(kKsOneshotReconnect, "RECONNECT");
        if (sent > 0) {
            // Every link that came up did so within 1.6-4.5 s (2026-11-21, 8
            // handovers). When the AirPods ignore the request (a phone still
            // holds them), waiting longer does not help; the caller's retry
            // does. 10 s here made those handovers take 13-15 s.
            constexpr auto kLinkWait = std::chrono::seconds{6};
            while (std::chrono::steady_clock::now() - t0 < kLinkWait) {
                bool up = false;
                try { up = device.ConnectionStatus() == BluetoothConnectionStatus::Connected; } catch (...) {}
                if (up) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    log::handover("CONNECT driver-level connect: link up after {} ms", ms);
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            log::handover("CONNECT driver-level connect: no link within 6 s");
            return false;
        }
        log::handover("CONNECT driver-level connect unavailable — falling back to SDP/RFCOMM wake");
    }

    // Step 1: bring up the BT classical link via uncached SDP / RFCOMM.
    bool socketOpened = false;
    bool linkUp = false;
    try {
        log::handover("CONNECT SDP probe (uncached) starting...");
        auto services = device.GetRfcommServicesAsync(BluetoothCacheMode::Uncached).get();
        log::handover("CONNECT SDP probe done — {} RFCOMM service(s)", services.Services().Size());

        // Socket connect with 3-second timeout on the first RFCOMM service only.
        // Previously this blocked for up to 21s when the phone's HFP service hadn't fully
        // released — long enough for the AirPods to idle-drop before A2DP could open.
        // A short timeout avoids the block while still sending the HFP wake signal when
        // the service is available.
        if (services.Services().Size() > 0) {
            auto svc     = services.Services().GetAt(0);
            auto host    = svc.ConnectionHostName();
            auto svcName = svc.ConnectionServiceName();
            log::handover("CONNECT RFCOMM socket connect (3s timeout)...");
            StreamSocket sock;
            std::atomic<int> sockResult{0}; // 0=pending 1=ok -1=fail
            std::thread t([&sock, host, svcName, &sockResult]() {
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    sock.ConnectAsync(host, svcName).get();
                    sockResult.store(1);
                } catch (...) { sockResult.store(-1); }
            });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (sockResult.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (sockResult.load() == 1) {
                socketOpened = true;
                log::handover("CONNECT RFCOMM socket opened — ACL link up");
                sock.Close();
            } else {
                const char* reason = (sockResult.load() == 0) ? "timed out (3s)" : "failed";
                log::handover("CONNECT RFCOMM socket {} — continuing with SDP wake only", reason);
                sock.Close(); // cancels the pending connect if still in flight
            }
            t.join();
        } else {
            log::handover("CONNECT 0 RFCOMM services — SDP probe alone is the wake trigger");
        }
    } catch (const winrt::hresult_error& e) {
        log::handover("CONNECT SDP/socket phase failed: 0x{:08X} {}",
            (std::uint32_t)e.code().value, winrt::to_string(e.message()));
    }

    try {
        linkUp = (device.ConnectionStatus() == BluetoothConnectionStatus::Connected);
        log::handover("CONNECT ACL connection status after wake: {} (linkUp={})",
            (int)device.ConnectionStatus(), linkUp);
    } catch (...) {}

    // Keepalive is handled by the SDP probe thread in startKeepalive().
    // The RFCOMM socket above was a wake trigger only — we don't maintain it.

    const bool kicked = socketOpened || linkUp;
    log::handover("CONNECT done (socketOpened={}, linkUp={})",
                  socketOpened, linkUp);
    return kicked;
}

namespace {

using Microsoft::WRL::ComPtr;

// Lowercase ASCII fold for case-insensitive substring search.
std::wstring foldLower(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + (L'a' - L'A'));
        out.push_back(c);
    }
    return out;
}

const char* stateName(DWORD s) {
    switch (s) {
        case DEVICE_STATE_ACTIVE:     return "ACTIVE";
        case DEVICE_STATE_DISABLED:   return "DISABLED";
        case DEVICE_STATE_NOTPRESENT: return "NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED:  return "UNPLUGGED";
        default:                      return "??";
    }
}

struct Endpoint {
    std::wstring id;
    DWORD state;
    std::wstring name;  // friendly name, for stereo-vs-HFP preference
};

// Enumerate endpoints in ALL states (active+disabled+notpresent+unplugged) and
// return ones whose friendly name (case-insensitive) contains "airpods".
std::vector<Endpoint> findAirPodsEndpoints(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    std::vector<Endpoint> hits;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &col))) return hits;

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(dev->GetId(&rawId)) || !rawId) continue;
        std::wstring idStr = rawId;
        CoTaskMemFree(rawId);

        DWORD state = 0;
        dev->GetState(&state);

        ComPtr<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) continue;

        PROPVARIANT name;
        PropVariantInit(&name);
        std::wstring friendly;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            friendly = name.pwszVal;
        }
        PropVariantClear(&name);

        if (friendly.empty()) continue;

        const bool isAirPods = foldLower(friendly).find(L"airpods") != std::wstring::npos;
        if (isAirPods) {
            log::debug("    [match] endpoint flow={} state={} name='{}'",
                flow == eRender ? "render" : "capture",
                stateName(state),
                winrt::to_string(friendly));
            hits.push_back({idStr, state, friendly});
        }
        // Non-AirPods endpoint debug lines omitted — they flood the log on
        // every IMMNotificationClient callback (dozens of devices × every
        // device-state-change event). Re-enable if debugging routing issues.
    }
    return hits;
}

// Update Teams' cmd_settings.json so it uses the specified audio endpoints.
// New Teams (Store edition) stores fixed device IDs here and does not expose a
// "system default" option in its UI. Writing these IDs before the user answers
// the incoming call routes Teams audio to AirPods for that session.
void updateTeamsDeviceConfig(const std::wstring& renderEpId, const std::wstring& captureEpId) {
    wchar_t localAppData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH)) return;

    const std::filesystem::path cfgPath =
        std::filesystem::path{localAppData}
        / L"Packages" / L"MSTeams_8wekyb3d8bbwe"
        / L"LocalCache" / L"Microsoft" / L"MSTeams" / L"cmd_settings.json";

    if (!std::filesystem::exists(cfgPath)) {
        log::debug("Teams cmd_settings.json not found — skipping Teams device update");
        return;
    }

    std::string content;
    {
        std::ifstream in(cfgPath);
        if (!in.is_open()) {
            log::warn("Cannot open Teams cmd_settings.json for reading");
            return;
        }
        content.assign(std::istreambuf_iterator<char>(in), {});
    }

    // Device IDs consist of ASCII hex digits, braces, dots, and dashes — safe to narrow.
    const std::string render  = winrt::to_string(renderEpId);
    const std::string capture = winrt::to_string(captureEpId);

    try {
        // Replace calling_selected_speaker_device value
        content = std::regex_replace(content,
            std::regex(R"("calling_selected_speaker_device":\{"Speaker":"[^"]*"\})"),
            "\"calling_selected_speaker_device\":{\"Speaker\":\"" + render + "\"}");

        // Replace calling_selected_microphone_device value
        content = std::regex_replace(content,
            std::regex(R"("calling_selected_microphone_device":\{"Microphone":"[^"]*"\})"),
            "\"calling_selected_microphone_device\":{\"Microphone\":\"" + capture + "\"}");

        // Replace Microphone+Speaker within calling_user_selected_devices, preserving Camera etc.
        content = std::regex_replace(content,
            std::regex(R"("calling_user_selected_devices":\{"Microphone":"[^"]*","Speaker":"[^"]*")"),
            "\"calling_user_selected_devices\":{\"Microphone\":\"" + capture
            + "\",\"Speaker\":\"" + render + "\"");
    } catch (const std::regex_error& e) {
        log::warn("Teams config regex error: {}", e.what());
        return;
    }

    {
        std::ofstream out(cfgPath, std::ios::trunc);
        if (!out.is_open()) {
            log::warn("Cannot write Teams cmd_settings.json");
            return;
        }
        out << content;
    }
    log::info("Teams device config updated — speaker={} mic={}", render, capture);
}

// Apply a single endpoint as the default for all three audio roles
// (Console, Multimedia, Communications). Logs per-role result. If the endpoint
// is not in DEVICE_STATE_ACTIVE, first calls SetEndpointVisibility to try to
// promote it. Returns true if at least one role ended up with this endpoint
// as default (either because we set it, or because it was already set).
//
// Idempotency: Windows' own audio policy auto-promotes a newly-connected BT
// device to default for some/all roles (depends on per-role level/rank state
// — see Microsoft Learn "Default Audio Endpoint Selection"). When it has
// already done so, our SetDefaultEndpoint call is a redundant write that
// causes a brief audio glitch and an extra OnDefaultDeviceChanged notification.
// Skip if GetDefaultAudioEndpoint(role) already returns our ID.
bool applyEndpointAsDefault(IMMDeviceEnumerator* enumerator,
                            IPolicyConfigVista* policy,
                            EDataFlow flow,
                            const Endpoint& ep,
                            const char* label) {
    if (ep.state != DEVICE_STATE_ACTIVE) {
        log::info("  {} endpoint is {} — making visible via SetEndpointVisibility",
            label, stateName(ep.state));
        HRESULT vr = policy->SetEndpointVisibility(ep.id.c_str(), 1);
        if (FAILED(vr)) {
            log::warn("  SetEndpointVisibility({}) failed: 0x{:08X}",
                label, (std::uint32_t)vr);
        }
    }

    // Helper: is `ep.id` currently the default endpoint for (flow, role)?
    auto alreadyDefault = [&](ERole role) -> bool {
        ComPtr<IMMDevice> cur;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &cur)) || !cur) {
            return false;
        }
        LPWSTR curIdRaw = nullptr;
        if (FAILED(cur->GetId(&curIdRaw)) || !curIdRaw) return false;
        std::wstring curId(curIdRaw);
        CoTaskMemFree(curIdRaw);
        return curId == ep.id;
    };

    bool anyOk = false;
    for (ERole role : { eConsole, eMultimedia, eCommunications }) {
        const char* roleName = role == eConsole ? "Console" :
                               role == eMultimedia ? "Multimedia" : "Communications";

        if (alreadyDefault(role)) {
            // Windows (or some other app) already pointed this role at us.
            // Skip the redundant SetDefaultEndpoint to avoid the brief glitch
            // and the spurious OnDefaultDeviceChanged notification.
            log::debug("AirPods already default {} ({} role) — skipping SetDefaultEndpoint",
                label, roleName);
            anyOk = true;
            continue;
        }

        HRESULT r = policy->SetDefaultEndpoint(ep.id.c_str(), role);
        if (SUCCEEDED(r)) {
            log::info("Set AirPods as default {} ({} role)", label, roleName);
            anyOk = true;
        } else {
            log::warn("SetDefaultEndpoint({}, {}) failed: 0x{:08X}",
                label, roleName, (std::uint32_t)r);
        }
    }
    return anyOk;
}

// Broadcast WM_WININICHANGE("Mmsys.cpl") — wakes apps registered for
// default-device-change notifications (Teams, Chrome, etc).
void broadcastDefaultDeviceChange() {
    DWORD recipients = BSM_APPLICATIONS;
    int sent = BroadcastSystemMessageW(
        BSF_POSTMESSAGE | BSF_IGNORECURRENTTASK,
        &recipients,
        WM_WININICHANGE,
        0,
        reinterpret_cast<LPARAM>(L"Mmsys.cpl"));
    log::debug("BroadcastSystemMessage(Mmsys.cpl) sent={}", sent);
}

// Single-pass routing: enumerate AirPods endpoints, apply any that are ACTIVE,
// broadcast the change, and patch Teams config. Returns true if any side was
// applied. Safe to call from any thread that has CoInitialize'd.
bool tryApplyAirPodsRouting(IMMDeviceEnumerator* enumerator,
                            IPolicyConfigVista* policy) {
    auto renderHits  = findAirPodsEndpoints(enumerator, eRender);
    auto captureHits = findAirPodsEndpoints(enumerator, eCapture);

    log::handover("ROUTE   found {} render + {} capture AirPods endpoint(s)",
        renderHits.size(), captureHits.size());
    for (auto& ep : renderHits)
        log::handover("ROUTE     render: state={} name='{}'",
            stateName(ep.state), winrt::to_string(ep.name));
    for (auto& ep : captureHits)
        log::handover("ROUTE     capture: state={} name='{}'",
            stateName(ep.state), winrt::to_string(ep.name));

    bool anyApplied = false;
    std::wstring activeRenderId, activeCaptureId;

    for (auto& ep : renderHits) {
        if (ep.state != DEVICE_STATE_ACTIVE) continue;
        if (applyEndpointAsDefault(enumerator, policy, eRender, ep, "render (output)")) {
            anyApplied = true;
            if (activeRenderId.empty()) activeRenderId = ep.id;
        }
    }
    for (auto& ep : captureHits) {
        if (ep.state != DEVICE_STATE_ACTIVE) continue;
        if (applyEndpointAsDefault(enumerator, policy, eCapture, ep, "capture (input)")) {
            anyApplied = true;
            if (activeCaptureId.empty()) activeCaptureId = ep.id;
        }
    }

    // UNPLUGGED fallback: try the stereo UNPLUGGED endpoint — forces Windows to open A2DP.
    if (!anyApplied) {
        log::handover("ROUTE   no ACTIVE endpoint — trying UNPLUGGED stereo fallback");
        auto isHandsFree = [](const Endpoint& e) {
            return foldLower(e.name).find(L"hands-free") != std::wstring::npos
                || foldLower(e.name).find(L"hands free") != std::wstring::npos;
        };
        for (auto& ep : renderHits) {
            if (ep.state != DEVICE_STATE_UNPLUGGED) continue;
            if (isHandsFree(ep)) continue;
            log::handover("ROUTE   applying UNPLUGGED stereo endpoint: '{}'",
                winrt::to_string(ep.name));
            if (applyEndpointAsDefault(enumerator, policy, eRender, ep, "render (output, stereo UNPLUGGED)")) {
                anyApplied = true;
                if (activeRenderId.empty()) activeRenderId = ep.id;
                break;
            }
        }
        if (!anyApplied)
            log::handover("ROUTE   no UNPLUGGED stereo endpoint found — routing not applied");
    }

    if (anyApplied) {
        log::handover("ROUTE   routing applied — broadcasting device change");
        broadcastDefaultDeviceChange();
        if (!activeRenderId.empty() || !activeCaptureId.empty()) {
            updateTeamsDeviceConfig(activeRenderId, activeCaptureId);
        }
    }
    return anyApplied;
}

// IMMNotificationClient that listens for endpoint state changes and applies
// AirPods routing the moment the OS promotes an AirPods endpoint to ACTIVE.
// This is the Windows analogue of Android's AudioDeviceCallback.
//
// Two modes:
//
//   TRANSIENT (default): arm() opens a 120-second window. While armed, any
//   AirPods endpoint transition to ACTIVE triggers routing; once routing
//   applies, the notifier disarms itself. If the window expires before any
//   ACTIVE transition, a diagnostic warning fires.
//
//   PERSISTENT (set via setPersistent(true)): the arm never expires, and
//   re-arms automatically after each successful routing. Used while we hold
//   OwnershipState::LocalPc — so when AirPods bounce off Windows mid-call
//   (Apple auto-switch to the phone, brief BT blips) and then come back, the
//   routing reapplies without needing another setAsDefaultAudioDevice() call.
//   HandoverController toggles this with the LocalPc/RemoteAndroid state.
//
// 120s transient window is sized for the worst-case observed reconnect race:
// after we call connect(), AirPods can bounce to the phone for ~30s before
// settling on Windows. A shorter window (the old 30s) frequently expired
// milliseconds before the ACTIVE transition arrived.
//
// Lifetime: singleton, registered once and leaked at process exit
// (the WASAPI samples and most production code do the same).
class EndpointNotifier : public IMMNotificationClient {
public:
    static constexpr std::chrono::seconds kArmWindow{120};

    void arm() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_armed = true;
        m_armedAt = std::chrono::steady_clock::now();
        log::debug("EndpointNotifier: armed ({}s window, persistent={})",
            kArmWindow.count(), m_persistent ? "yes" : "no");
    }

    // Persistent mode: armed indefinitely until cleared. After a successful
    // routing application, the notifier re-arms (instead of disarming) so
    // further UNPLUGGED→ACTIVE cycles also route. Called by HandoverController
    // when transitioning to/from OwnershipState::LocalPc.
    void setPersistent(bool persistent) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_persistent == persistent) return;
        m_persistent = persistent;
        log::info("EndpointNotifier: persistent mode {}", persistent ? "ENABLED" : "DISABLED");
        if (persistent) {
            // Auto-arm so the caller doesn't have to also call arm().
            m_armed = true;
            m_armedAt = std::chrono::steady_clock::now();
        } else {
            // Disarm immediately when leaving persistent mode. Without this, the old
            // arm window (up to 120 s) can outlive ownership: HandoverController calls
            // setPersistent(false) when transitioning to RemoteAndroid, but if we just
            // clear m_persistent the next AirPods UNPLUGGED→ACTIVE bounce within the
            // residual window still satisfies isArmedAndFresh() and re-routes audio
            // back to Windows — fighting the peer that just took ownership. This is
            // the symmetric companion to the auto-arm on persistent=true above; it
            // matches the documented intent ("drop the persistent flag so we don't
            // fight a peer that is intentionally taking the AirPods").
            m_armed = false;
        }
    }

    bool isArmedAndFresh() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_armed) return false;
        if (m_persistent) return true;  // never expires in persistent mode
        auto age = std::chrono::steady_clock::now() - m_armedAt;
        if (age >= kArmWindow) {
            m_armed = false;
            log::warn("EndpointNotifier: arm window expired ({}s) — AirPods endpoint never reached ACTIVE. "
                      "A2DP profile activation has failed at the OS level, or the OS reconnect race "
                      "took longer than the window. Check the [BT-diag] log lines above.",
                      kArmWindow.count());
            return false;
        }
        return true;
    }

    void setEnumerator(ComPtr<IMMDeviceEnumerator> e) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_enumerator = std::move(e);
    }

    // IUnknown — singleton; ref-counting is a no-op for lifetime purposes.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return InterlockedDecrement(&m_ref);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override {
        if (!pwstrDeviceId) return S_OK;
        log::debug("IMMNotificationClient::OnDeviceStateChanged: id='{}' -> {}",
            winrt::to_string(pwstrDeviceId), stateName(dwNewState));
        if (dwNewState == DEVICE_STATE_ACTIVE) {
            maybeApplyForDevice(pwstrDeviceId);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
        if (!pwstrDeviceId) return S_OK;
        log::debug("IMMNotificationClient::OnDeviceAdded: id='{}'",
            winrt::to_string(pwstrDeviceId));
        maybeApplyForDevice(pwstrDeviceId);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG m_ref = 1;
    std::mutex m_mtx;
    bool m_armed = false;
    bool m_persistent = false;
    std::chrono::steady_clock::time_point m_armedAt;
    ComPtr<IMMDeviceEnumerator> m_enumerator;

    // If the device named in the callback is an AirPods endpoint and we are
    // armed, run the full routing pass. Held lock released before heavy work
    // (CoCreateInstance, SetDefaultEndpoint) so we don't serialize callbacks.
    void maybeApplyForDevice(LPCWSTR id) {
        if (!isArmedAndFresh()) return;

        ComPtr<IMMDeviceEnumerator> enumerator;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            enumerator = m_enumerator;
        }
        if (!enumerator) return;

        // Quick filter: only proceed if this id is actually an AirPods endpoint.
        // The callback fires for every audio device state change on the system
        // (USB mics, HDMI displays, etc.), so we don't want to do COM heavy
        // lifting unless this is one of ours.
        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator->GetDevice(id, &dev))) return;
        ComPtr<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return;
        PROPVARIANT name;
        PropVariantInit(&name);
        std::wstring friendly;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal) {
            friendly = name.pwszVal;
        }
        PropVariantClear(&name);
        if (foldLower(friendly).find(L"airpods") == std::wstring::npos) return;

        log::handover("ROUTE   AirPods endpoint '{}' just hit ACTIVE — routing now (notifier)",
            winrt::to_string(friendly));

        ComPtr<IPolicyConfigVista> policy;
        HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigVistaClient, nullptr, CLSCTX_ALL,
            __uuidof(IPolicyConfigVista), (void**)policy.GetAddressOf());
        if (FAILED(hr)) {
            log::warn("Notifier: CoCreateInstance(IPolicyConfigVista) failed: 0x{:08X}",
                (std::uint32_t)hr);
            return;
        }

        bool applied = tryApplyAirPodsRouting(enumerator.Get(), policy.Get());
        if (applied) {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_persistent) {
                // Stay armed so subsequent UNPLUGGED→ACTIVE cycles also route
                // (e.g. AirPods bounce to phone briefly mid-call and come back).
                m_armedAt = std::chrono::steady_clock::now();
                log::info("Endpoint notifier: routing applied — staying armed (persistent mode)");
            } else {
                m_armed = false;
                log::info("Endpoint notifier: routing applied — disarmed");
            }
        }
    }
};

// Singleton notifier. Created on first use, leaked at process exit.
EndpointNotifier* g_notifier = nullptr;
std::once_flag g_notifierInit;

// Register the singleton notifier with the audio system the first time we're
// called; refresh its enumerator pointer every call so subsequent runs use
// the latest CoCreateInstance result.
EndpointNotifier* ensureNotifier(IMMDeviceEnumerator* enumerator) {
    std::call_once(g_notifierInit, [enumerator]() {
        g_notifier = new EndpointNotifier();
        HRESULT hr = enumerator->RegisterEndpointNotificationCallback(g_notifier);
        if (FAILED(hr)) {
            log::error("RegisterEndpointNotificationCallback failed: 0x{:08X}",
                (std::uint32_t)hr);
            delete g_notifier;
            g_notifier = nullptr;
            return;
        }
        log::info("Endpoint notification callback registered — event-driven AirPods routing active");
    });
    if (g_notifier) {
        ComPtr<IMMDeviceEnumerator> copy = enumerator;
        g_notifier->setEnumerator(std::move(copy));
    }
    return g_notifier;
}

}

// Event-driven: arms the IMMNotificationClient (registered lazily on first
// call) so that the moment any AirPods endpoint transitions to ACTIVE, the
// full routing pass fires. Also does a synchronous pass for the case where
// the endpoint is already ACTIVE by the time we get here (e.g. retry after
// a 3s delay in HandoverController). Returns immediately — no blocking poll.
//
// This is the Windows equivalent of Android's AudioDeviceCallback:
// MediaController.kt registers `audioManager.registerAudioDeviceCallback(...)`
// and waits for `onAudioDevicesAdded(TYPE_BLUETOOTH_A2DP)` to fire. Here we
// register an IMMNotificationClient and wait for OnDeviceStateChanged on an
// AirPods endpoint to fire with DEVICE_STATE_ACTIVE.
bool AirPodsConnector::setAsDefaultAudioDevice() {
    // One-shot synchronous routing: if the AirPods endpoint is already ACTIVE, make them
    // the default output/input device immediately via SetDefaultEndpoint. No IMMNotification-
    // Client arm, no 3s re-assert, no routing loop — just one quiet pass. Required because
    // Windows doesn't always auto-promote the AirPods to default when another audio device
    // is already the output (e.g. PC speakers, Xiaomi overnight, etc.), leaving the endpoint
    // ACTIVE but no audio flowing → idle-drop. If the endpoint is not ACTIVE yet (still
    // UNPLUGGED), this is a no-op and returns true (caller retries via HandoverController).
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInited = (coInitHr == S_OK);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)enumerator.GetAddressOf());
    if (FAILED(hr)) {
        log::warn("CoCreateInstance(MMDeviceEnumerator) failed: 0x{:08X}", (std::uint32_t)hr);
        if (weInited) CoUninitialize();
        return false;
    }

    ComPtr<IPolicyConfigVista> policy;
    hr = CoCreateInstance(CLSID_CPolicyConfigVistaClient, nullptr, CLSCTX_ALL,
                          __uuidof(IPolicyConfigVista), (void**)policy.GetAddressOf());
    if (FAILED(hr)) {
        log::warn("CoCreateInstance(IPolicyConfigVista) failed: 0x{:08X}", (std::uint32_t)hr);
        if (weInited) CoUninitialize();
        return false;
    }

    // Synchronous pass only — no IMMNotificationClient notifier arm. The notifier-based
    // routing caused continuous churn; a single pass here is sufficient. If the endpoint
    // is ACTIVE we set default now; if not yet ACTIVE, HandoverController's 3s retry fires
    // this again by which point the endpoint should have settled.
    log::handover("ROUTE   setAsDefaultAudioDevice called — running routing pass");
    bool appliedNow = tryApplyAirPodsRouting(enumerator.Get(), policy.Get());

    // Arm the IMMNotificationClient so when A2DP opens (UNPLUGGED→ACTIVE) we route
    // immediately on the event thread. Required when endpoint is UNPLUGGED at call time —
    // SetDefaultEndpoint on UNPLUGGED alone doesn't reliably open A2DP fast enough to
    // prevent the AirPods from idle-dropping before audio flows.
    EndpointNotifier* notifier = ensureNotifier(enumerator.Get());
    if (notifier) {
        notifier->arm();
        log::handover("ROUTE   notifier armed — will route when endpoint goes ACTIVE (120s window)");
    }

    log::handover("ROUTE   setAsDefaultAudioDevice done — appliedNow={}", appliedNow);

    if (weInited) CoUninitialize();
    return true;
}

void AirPodsConnector::setPersistentArm(bool persistent) {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInited = (coInitHr == S_OK);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)enumerator.GetAddressOf()))) {
        EndpointNotifier* notifier = ensureNotifier(enumerator.Get());
        if (notifier) {
            notifier->setPersistent(persistent);
            log::handover("ROUTE   setPersistentArm({})", persistent ? "true" : "false");
        }
    }

    if (weInited) CoUninitialize();
}

bool AirPodsConnector::hasActiveAudioSessions() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInited = (coInitHr == S_OK);

    bool active = false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)enumerator.GetAddressOf());
    if (SUCCEEDED(hr)) {
        // Check the render (output) endpoint — calls and meetings produce output audio.
        for (auto& ep : findAirPodsEndpoints(enumerator.Get(), eRender)) {
            if (ep.state != DEVICE_STATE_ACTIVE) continue;
            ComPtr<IMMDevice> dev;
            if (FAILED(enumerator->GetDevice(ep.id.c_str(), &dev))) continue;

            ComPtr<IAudioSessionManager2> mgr;
            if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                     nullptr, (void**)mgr.GetAddressOf()))) continue;

            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(mgr->GetSessionEnumerator(&sessions))) continue;

            int count = 0;
            sessions->GetCount(&count);
            for (int i = 0; i < count && !active; ++i) {
                ComPtr<IAudioSessionControl> ctrl;
                if (FAILED(sessions->GetSession(i, &ctrl))) continue;
                AudioSessionState state{};
                if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive) {
                    active = true;
                    log::debug("hasActiveAudioSessions: found active session on AirPods render endpoint");
                }
            }
            if (active) break;
        }
    }

    if (weInited) CoUninitialize();
    return active;
}

namespace {

// Process names that, when actively playing audio on the AirPods endpoint,
// indicate a real-time call/meeting that the user does not want a peer device
// to interrupt by snatching the AirPods. Music apps (Spotify, Apple Music,
// Chrome playing YouTube, etc.) are deliberately NOT on this list: they don't
// warrant blocking a user-initiated handover to their phone.
//
// Mirrors the smaller list in TeamsCallWatcher.cpp's isCommsApp(); we keep a
// local copy here rather than sharing a header because the two callers have
// different intents (the watcher uses it to trigger handover; we use it to
// gate a release). They can diverge.
bool isCommsAppExe(const std::wstring& exe) {
    constexpr const wchar_t* kCommsExes[] = {
        L"ms-teams.exe",        // New Teams (Win32)
        L"Teams.exe",           // Classic Teams
        L"zoom.exe",            // Zoom desktop
        L"Discord.exe",         // Discord
        L"slack.exe",           // Slack (calls)
        L"webex.exe",           // Cisco WebEx
        L"webexd.exe",          // Cisco WebEx daemon
    };
    for (const auto* name : kCommsExes) {
        if (_wcsicmp(exe.c_str(), name) == 0) return true;
    }
    return false;
}

// Look up the executable basename for a process id. Returns empty string on
// failure (process gone, access denied, etc.). Uses PROCESS_QUERY_LIMITED_-
// INFORMATION which works for processes running at higher integrity than ours.
std::wstring getProcessExeBasename(DWORD pid) {
    if (pid == 0) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &len);
    CloseHandle(hProc);
    if (!ok) return {};
    std::wstring full{path};
    auto slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? full : full.substr(slash + 1);
}

}  // anonymous

bool AirPodsConnector::hasActiveCallSessions() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInited = (coInitHr == S_OK);

    bool callActive = false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)enumerator.GetAddressOf());
    if (!SUCCEEDED(hr)) {
        log::handover("CALLVETO CoCreateInstance(MMDeviceEnumerator) failed: 0x{:08X}", (std::uint32_t)hr);
        if (weInited) CoUninitialize();
        return false;
    }

    auto endpoints = findAirPodsEndpoints(enumerator.Get(), eRender);
    log::handover("CALLVETO found {} AirPods render endpoint(s)", endpoints.size());

    for (auto& ep : endpoints) {
        if (ep.state != DEVICE_STATE_ACTIVE && ep.state != DEVICE_STATE_UNPLUGGED) {
            log::handover("CALLVETO   ep='{}' state={} — skip (not ACTIVE or UNPLUGGED)",
                winrt::to_string(ep.name), stateName(ep.state));
            continue;
        }
        log::handover("CALLVETO   ep='{}' state={} — checking...",
            winrt::to_string(ep.name), stateName(ep.state));

        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator->GetDevice(ep.id.c_str(), &dev))) {
            log::handover("CALLVETO   GetDevice failed");
            continue;
        }

        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                 nullptr, (void**)mgr.GetAddressOf()))) {
            log::handover("CALLVETO   Activate(IAudioSessionManager2) failed");
            continue;
        }

        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(mgr->GetSessionEnumerator(&sessions))) {
            log::handover("CALLVETO   GetSessionEnumerator failed");
            continue;
        }

        int count = 0;
        sessions->GetCount(&count);
        log::handover("CALLVETO   {} session(s) on endpoint", count);

        for (int i = 0; i < count && !callActive; ++i) {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessions->GetSession(i, &ctrl))) continue;
            AudioSessionState state{};
            if (FAILED(ctrl->GetState(&state)) || state != AudioSessionStateActive) continue;

            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl.As(&ctrl2))) continue;
            if (ctrl2->IsSystemSoundsSession() == S_OK) continue;

            DWORD pid = 0;
            if (FAILED(ctrl2->GetProcessId(&pid)) || pid == 0) continue;

            std::wstring exe = getProcessExeBasename(pid);
            if (exe.empty()) continue;

            log::handover("CALLVETO   session pid={} exe='{}' state=Active",
                pid, winrt::to_string(exe));

            if (isCommsAppExe(exe)) {
                callActive = true;
                log::handover("CALLVETO   -> MATCH: '{}' is a comms app — call veto ACTIVE",
                    winrt::to_string(exe));
            }
        }
        if (callActive) break;
    }

    if (!callActive)
        log::handover("CALLVETO -> NO active comms session found — veto will NOT fire");

    if (weInited) CoUninitialize();
    return callActive;
}

bool AirPodsConnector::isClassicallyConnected() {
    if (m_address == 0) return false;
    HANDLE hRadio = openFirstRadio();
    if (!hRadio) return false;
    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof(info);
    info.Address.ullLong = m_address;
    DWORD err = BluetoothGetDeviceInfo(hRadio, &info);
    CloseHandle(hRadio);
    return err == ERROR_SUCCESS && info.fConnected;
}

bool AirPodsConnector::isClassicallyConnectedWinRT() {
    if (m_address == 0) return false;
    try {
        using namespace winrt::Windows::Devices::Bluetooth;
        auto device = BluetoothDevice::FromBluetoothAddressAsync(m_address).get();
        return device != nullptr
            && device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

bool AirPodsConnector::disconnect() {
    if (m_address == 0) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    log::debug("AirPodsConnector::disconnect() address={:012X}", m_address);

    // Early-out if the AirPods are already disconnected at the BT layer. This is
    // the common case during peer-initiated handover: by the time we process
    // kRequestDisconnect, the peer (Pixel/Xiaomi) has already grabbed the AirPods
    // and Apple's auto-switch has dropped our connection.
    // Uses WinRT check to avoid bthserv RPC.
    if (!isClassicallyConnectedWinRT()) {
        log::handover("DISCONNECT AirPods already disconnected at BT layer — "
                      "no BT profile state changes needed");
        return true;
    }

    // Release with the Bluetooth audio driver's own disconnect
    // (KSPROPERTY_ONESHOT_DISCONNECT), the same request as Settings ->
    // Disconnect. It frees the AirPods for the phone right away instead of
    // leaving the phone to win a page race against a PC that still holds them.
    // It does not change any persisted profile state.
    //
    // NEVER use BluetoothSetServiceState(DISABLE) here: Windows persists it, so
    // the next connection comes up with A2DP NOTPRESENT (no sound) until it is
    // re-enabled on a cold link. That was the "connects but no audio" bug.
    const auto t0 = std::chrono::steady_clock::now();
    const int sent = sendBtAudioOneshot(kKsOneshotDisconnect, "DISCONNECT");
    if (sent == 0) {
        log::info("AirPods release for {:012X}: driver-level disconnect unavailable; "
                  "leaving the link for the peer's A2DP grab", m_address);
        return true;
    }

    // The audio disconnect closes the profiles, but Windows then keeps the
    // Bluetooth link itself up for another ~3.5 s (2026-11-21: link down after
    // 3.4-3.6 s), which the phone has to wait out. Drop the link right away
    // with the Bluetooth stack's own disconnect (IOCTL_BTH_DISCONNECT_DEVICE,
    // bthioctl.h). It only ends this connection; nothing is persisted, and the
    // AirPods stay paired. The profiles were closed first, so audio ends
    // cleanly rather than as a lost link.
    {
        constexpr DWORD kIoctlBthDisconnectDevice =
            CTL_CODE(0x41 /* FILE_DEVICE_BLUETOOTH */, 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS);
        HANDLE hRadio = openFirstRadio();
        if (hRadio) {
            ULONGLONG addr = m_address;
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(hRadio, kIoctlBthDisconnectDevice,
                                            &addr, sizeof(addr), nullptr, 0, &returned, nullptr);
            const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            CloseHandle(hRadio);
            log::handover("DISCONNECT link drop (IOCTL_BTH_DISCONNECT_DEVICE): {}",
                          ok ? std::string("ok") : std::format("error {}", err));
        } else {
            log::handover("DISCONNECT link drop skipped: no Bluetooth radio handle");
        }
    }

    // The caller tells the peers "released" (kAirPodsDisconnected) as soon as
    // this returns, and a phone connects right after that. The driver request
    // returns in ~0.25 s, before the link is down; a phone page that lands
    // during the teardown goes unanswered until Android's 30 s page timeout
    // (2026-11-21: PC -> Pixel took 28 s). So wait, bounded, for the link to
    // actually drop before returning. 3 s was not always enough while VLC
    // was streaming (2026-11-21); the phone waits 6 s for this ack.
    constexpr auto kLinkDownWait = std::chrono::seconds{5};
    try {
        using namespace winrt::Windows::Devices::Bluetooth;
        auto device = BluetoothDevice::FromBluetoothAddressAsync(m_address).get();
        while (device && std::chrono::steady_clock::now() - t0 < kLinkDownWait) {
            if (device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                log::handover("DISCONNECT link down after {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count());
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        log::handover("DISCONNECT link still up after 5 s; releasing anyway");
    } catch (const winrt::hresult_error& e) {
        log::handover("DISCONNECT link-down wait failed (0x{:08X}); releasing anyway",
                      static_cast<uint32_t>(e.code()));
    }
    return true;
}


namespace {

// KSPROPSETID_BtAudio {7FA06C40-B8F6-4C7E-8556-E8C33A12E54D} (ksmedia.h).
constexpr GUID kBtAudioSet = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};

// IKsControl over a raw handle to a KS filter's device interface, for drivers
// whose topology does not hand out IKsControl through COM (the test PC).
class HandleKsControl final : public IKsControl {
public:
    explicit HandleKsControl(HANDLE h) : m_h(h) {}
    ~HandleKsControl() { if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IKsControl)) {
            *ppv = static_cast<IKsControl*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --m_refs;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData,
                            ULONG DataLength, ULONG* BytesReturned) override {
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(m_h, IOCTL_KS_PROPERTY, Property, PropertyLength,
                                        PropertyData, DataLength, &bytes, nullptr);
        if (BytesReturned) *BytesReturned = bytes;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return E_NOTIMPL; }

private:
    HANDLE m_h;
    std::atomic<ULONG> m_refs{1};
};

// endpoint -> topology connector 0 -> connected filter -> IKsControl. Tries
// the COM routes first, then opens the filter's device interface directly
// (the route that worked on the test PC). `key` receives the filter id.
HRESULT acquireBtAudioKs(IMMDeviceEnumerator* en, IMMDevice* dev, ComPtr<IKsControl>& ks,
                         std::wstring& key, std::string& note) {
    ComPtr<IDeviceTopology> topo;
    HRESULT hr = dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(topo.GetAddressOf()));
    if (FAILED(hr)) { note = std::format("Activate(IDeviceTopology) 0x{:08X}", (std::uint32_t)hr); return hr; }
    ComPtr<IConnector> conn;
    hr = topo->GetConnector(0, &conn);
    if (FAILED(hr)) { note = std::format("GetConnector(0) 0x{:08X}", (std::uint32_t)hr); return hr; }
    LPWSTR connId = nullptr;
    if (SUCCEEDED(conn->GetDeviceIdConnectedTo(&connId)) && connId) {
        key = connId;
        CoTaskMemFree(connId);
    }
    ComPtr<IConnector> other;
    hr = conn->GetConnectedTo(&other);
    if (FAILED(hr)) { note = std::format("GetConnectedTo 0x{:08X}", (std::uint32_t)hr); return hr; }

    ComPtr<IPart> part;
    if (SUCCEEDED(other.As(&part)) &&
        SUCCEEDED(part->Activate(CLSCTX_ALL, __uuidof(IKsControl), reinterpret_cast<void**>(ks.GetAddressOf())))) {
        note = "IPart";
        return S_OK;
    }
    if (!key.empty()) {
        ComPtr<IMMDevice> filter;
        if (SUCCEEDED(en->GetDevice(key.c_str(), &filter)) &&
            SUCCEEDED(filter->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(ks.GetAddressOf())))) {
            note = "GetDevice";
            return S_OK;
        }
        // "{2}.\\?\<device interface path>"
        if (const auto dot = key.find(L".\\\\?\\"); dot != std::wstring::npos) {
            const std::wstring path = key.substr(dot + 1);
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                ks.Attach(new HandleKsControl(h));
                note = "device interface";
                return S_OK;
            }
            hr = HRESULT_FROM_WIN32(GetLastError());
            note = std::format("CreateFile 0x{:08X}", (std::uint32_t)hr);
            return hr;
        }
    }
    note = "no IKsControl route";
    return E_NOINTERFACE;
}

// Sends one KSPROPSETID_BtAudio one-shot request (reconnect/disconnect) to
// every distinct Bluetooth audio filter behind the AirPods' render and capture
// endpoints (A2DP and hands-free). Returns how many filters accepted it.
// Success only means the driver started the operation; the link and endpoint
// state confirm the result.
int sendBtAudioOneshot(ULONG propertyId, const char* label) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        log::warn("KS {}: cannot create the audio device enumerator", label);
        return 0;
    }
    std::vector<Endpoint> endpoints = findAirPodsEndpoints(en.Get(), eRender);
    for (auto& e : findAirPodsEndpoints(en.Get(), eCapture)) endpoints.push_back(e);

    std::vector<std::wstring> done;
    int accepted = 0;
    for (const auto& ep : endpoints) {
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDevice(ep.id.c_str(), &dev))) continue;
        ComPtr<IKsControl> ks;
        std::wstring key;
        std::string note;
        if (FAILED(acquireBtAudioKs(en.Get(), dev.Get(), ks, key, note))) {
            log::debug("KS {}: '{}' has no Bluetooth audio control ({})", label,
                winrt::to_string(ep.name), note);
            continue;
        }
        if (std::find(done.begin(), done.end(), key) != done.end()) continue;
        done.push_back(key);
        KSPROPERTY prop{};
        prop.Set = kBtAudioSet;
        prop.Id = propertyId;
        prop.Flags = KSPROPERTY_TYPE_GET;
        ULONG bytes = 0;
        const HRESULT hr = ks->KsProperty(&prop, sizeof(prop), nullptr, 0, &bytes);
        log::handover("KS      ONESHOT_{} '{}' via {}: 0x{:08X}", label,
            winrt::to_string(ep.name), note, (std::uint32_t)hr);
        if (SUCCEEDED(hr)) ++accepted;
    }
    return accepted;
}

}  // namespace

}
