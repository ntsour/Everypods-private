// bt_wake_test.cpp — STANDALONE throwaway test (NOT part of the Propods build).
//
// Goal: prove that we can connect the AirPods to Windows on demand by ONLY waking
// the ACL link (uncached RFCOMM/SDP + opportunistic socket open) and letting Windows
// auto-activate the audio endpoint — with ZERO BluetoothSetServiceState calls.
//
// It prints every AirPods audio endpoint's DEVICE_STATE (ACTIVE/UNPLUGGED/NOTPRESENT/
// DISABLED) before and during a 30s poll, so we can watch the stereo A2DP endpoint go
// ACTIVE after the wake. If it reaches ACTIVE with no service-state toggling, the
// "let Windows manage audio" model is viable and we can strip BluetoothSetServiceState
// from AirPodsConnector.cpp.
//
// Build: windows\tools\build_wake_test.bat   (uses VS Build Tools cl.exe)
// Run:   bt_wake_test.exe [AABBCCDDEEFF | AA:BB:CC:DD:EE:FF]   (default = AirPods MAC below)
//
// IMPORTANT: this file must NEVER call BluetoothSetServiceState — that is the whole point.

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Networking.h>

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;

static const wchar_t* stateName(DWORD s) {
    switch (s) {
        case DEVICE_STATE_ACTIVE:     return L"ACTIVE";
        case DEVICE_STATE_DISABLED:   return L"DISABLED";
        case DEVICE_STATE_NOTPRESENT: return L"NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED:  return L"UNPLUGGED";
        default:                      return L"?";
    }
}

static std::wstring foldLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Print all AirPods audio endpoints (render + capture, all states) and return the
// DEVICE_STATE of the STEREO render endpoint ("AirPods Pro - Find My" without
// "Hands-Free"), or 0 if not found.
static DWORD dumpEndpoints(const char* tag) {
    DWORD stereoState = 0;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        printf("[%s] CoCreateInstance(MMDeviceEnumerator) failed\n", tag);
        return 0;
    }
    for (EDataFlow flow : { eRender, eCapture }) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &col))) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(col->Item(i, &dev))) continue;
            DWORD st = 0; dev->GetState(&st);
            ComPtr<IPropertyStore> props;
            std::wstring name;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR && pv.pwszVal) name = pv.pwszVal;
                PropVariantClear(&pv);
            }
            if (foldLower(name).find(L"airpods") == std::wstring::npos) continue;
            const wchar_t* fl = (flow == eRender) ? L"render " : L"capture";
            wprintf(L"[%S]   %s state=%-10s '%s'\n", tag, fl, stateName(st), name.c_str());
            const bool isHandsFree = foldLower(name).find(L"hands-free") != std::wstring::npos
                                  || foldLower(name).find(L"hands free") != std::wstring::npos;
            if (flow == eRender && !isHandsFree) stereoState = st;
        }
    }
    return stereoState;
}

static std::uint64_t parseMac(const char* s) {
    std::uint64_t mac = 0; int nibbles = 0;
    for (const char* p = s; *p && nibbles < 12; ++p) {
        char c = *p; int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue; // skip ':' '-' etc.
        mac = (mac << 4) | (std::uint64_t)v; ++nibbles;
    }
    return mac;
}

int wmain(int argc, wchar_t** argv) {
    // Default to the AirPods MAC seen in this session; override via argv[1].
    std::uint64_t mac = 0x307AD2039786ULL;
    if (argc > 1) {
        char buf[64] = {};
        wcstombs(buf, argv[1], sizeof(buf) - 1);
        std::uint64_t m = parseMac(buf);
        if (m) mac = m;
    }
    printf("bt_wake_test: target AirPods MAC = %012llX\n", (unsigned long long)mac);
    printf("(this program never calls BluetoothSetServiceState)\n\n");

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    printf("=== BASELINE endpoints (before wake) ===\n");
    dumpEndpoints("base");

    // --- WAKE THE LINK (no service-state toggling) ---
    printf("\n=== WAKE: FromBluetoothAddressAsync + uncached RFCOMM/SDP + socket open ===\n");
    try {
        auto device = BluetoothDevice::FromBluetoothAddressAsync(mac).get();
        if (!device) { printf("FromBluetoothAddressAsync returned null\n"); return 2; }
        printf("ConnectionStatus before: %d (1=Connected,0=Disconnected)\n",
               (int)device.ConnectionStatus());

        try {
            auto services = device.GetRfcommServicesAsync(BluetoothCacheMode::Uncached).get();
            printf("RFCOMM services discovered (uncached): %u\n", services.Services().Size());
            for (auto&& svc : services.Services()) {
                try {
                    StreamSocket sock;
                    sock.ConnectAsync(svc.ConnectionHostName(), svc.ConnectionServiceName()).get();
                    printf("  opened RFCOMM socket — ACL link should be up\n");
                    sock.Close();
                    break;
                } catch (winrt::hresult_error const& e) {
                    printf("  socket open failed: 0x%08X\n", (unsigned)e.code().value);
                }
            }
        } catch (winrt::hresult_error const& e) {
            printf("GetRfcommServicesAsync failed: 0x%08X (AirPods may advertise 0 RFCOMM svcs — "
                   "the SDP/connection attempt is still the wake)\n", (unsigned)e.code().value);
        }
        printf("ConnectionStatus after wake attempt: %d\n", (int)device.ConnectionStatus());
    } catch (winrt::hresult_error const& e) {
        printf("WAKE failed: 0x%08X\n", (unsigned)e.code().value);
    }

    // --- POLL endpoint states for 30s, log when the stereo endpoint changes ---
    printf("\n=== POLL (30s) — watching for stereo A2DP endpoint to reach ACTIVE ===\n");
    DWORD last = 0xFFFFFFFF;
    bool sawActive = false;
    for (int i = 0; i < 30; ++i) {
        DWORD stereo = 0;
        ComPtr<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&en)))) {
            ComPtr<IMMDeviceCollection> col;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &col))) {
                UINT n = 0; col->GetCount(&n);
                for (UINT k = 0; k < n; ++k) {
                    ComPtr<IMMDevice> dev; if (FAILED(col->Item(k, &dev))) continue;
                    ComPtr<IPropertyStore> props; std::wstring name;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                        PROPVARIANT pv; PropVariantInit(&pv);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                            pv.vt == VT_LPWSTR && pv.pwszVal) name = pv.pwszVal;
                        PropVariantClear(&pv);
                    }
                    std::wstring nl = foldLower(name);
                    if (nl.find(L"airpods") == std::wstring::npos) continue;
                    if (nl.find(L"hands-free") != std::wstring::npos ||
                        nl.find(L"hands free") != std::wstring::npos) continue;
                    dev->GetState(&stereo);
                }
            }
        }
        if (stereo != last) {
            wprintf(L"  t+%02ds  stereo endpoint -> %s\n", i, stateName(stereo));
            last = stereo;
        }
        if (stereo == DEVICE_STATE_ACTIVE) { sawActive = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    printf("\n=== RESULT ===\n");
    printf("%s — stereo A2DP endpoint %s ACTIVE via link-wake alone (no BluetoothSetServiceState)\n",
           sawActive ? "PASS" : "FAIL/INCONCLUSIVE",
           sawActive ? "REACHED" : "did NOT reach");
    printf("\nFinal endpoint snapshot:\n");
    dumpEndpoints("final");
    printf("\nIf PASS: play audio now to confirm sound. If the stereo endpoint is ACTIVE,\n"
           "the 'let Windows manage audio' model works and we can strip the service toggling.\n");
    return sawActive ? 0 : 1;
}
