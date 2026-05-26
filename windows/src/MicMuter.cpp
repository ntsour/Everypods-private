#include "MicMuter.hpp"
#include "Logger.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace librepods {

namespace {

// Returns the IAudioEndpointVolume for the default eCommunications mic,
// or nullptr on any failure (device absent, COM error, etc.).
ComPtr<IAudioEndpointVolume> getCommsMicVolume() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(
            eCapture, eCommunications, &device)))
        return nullptr;

    ComPtr<IAudioEndpointVolume> vol;
    device->Activate(__uuidof(IAudioEndpointVolume),
                     CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(vol.GetAddressOf()));
    return vol;
}

} // namespace

void MicMuter::toggle() {
    auto vol = getCommsMicVolume();
    if (!vol) {
        log::warn("MicMuter: eCommunications capture endpoint not available");
        return;
    }
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    vol->SetMute(!muted, nullptr);
    log::info("MicMuter: microphone {}", !muted ? "MUTED" : "unmuted");
}

bool MicMuter::isMuted() const {
    auto vol = getCommsMicVolume();
    if (!vol) return false;
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    return muted != FALSE;
}

} // namespace librepods
