#include "MicMuter.hpp"
#include "Logger.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <winrt/base.h>

#include <string>

using Microsoft::WRL::ComPtr;

namespace Propods {

namespace {

// Returns the IAudioEndpointVolume for the default eCommunications mic,
// or nullptr on any failure (device absent, COM error, etc.).
// Also logs the friendly name of the device so callers can see whether
// it's the AirPods mic (HFP active) or the laptop mic (HFP not yet up).
ComPtr<IAudioEndpointVolume> getCommsMicVolume(std::string* friendlyNameOut = nullptr) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(
            eCapture, eCommunications, &device)))
        return nullptr;

    // Read the friendly name for diagnostic logging.
    if (friendlyNameOut) {
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal) {
                *friendlyNameOut = winrt::to_string(std::wstring_view(name.pwszVal));
            }
            PropVariantClear(&name);
        }
    }

    ComPtr<IAudioEndpointVolume> vol;
    device->Activate(__uuidof(IAudioEndpointVolume),
                     CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(vol.GetAddressOf()));
    return vol;
}

} // namespace

void MicMuter::toggle() {
    std::string deviceName;
    auto vol = getCommsMicVolume(&deviceName);
    if (!vol) {
        log::warn("MicMuter: eCommunications capture endpoint not available "
                  "(AirPods HFP not active? Connect to a call first)");
        return;
    }
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    vol->SetMute(!muted, nullptr);
    log::info("MicMuter: [{}] {}", deviceName, !muted ? "MUTED" : "unmuted");
}

bool MicMuter::isMuted() const {
    auto vol = getCommsMicVolume();
    if (!vol) return false;
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    return muted != FALSE;
}

} // namespace Propods
