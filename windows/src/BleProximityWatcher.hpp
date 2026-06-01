#pragma once

#include <atomic>
#include <cstdint>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>

namespace Propods {

// Passive BLE proximity watcher for AirPods presence detection.
//
// Windows speaks only classic Bluetooth (A2DP/HFP) to the AirPods and has no
// AACP/L2CAP link, so it cannot read in-ear / case state directly. But AirPods
// continuously broadcast Apple's unencrypted "proximity pairing" BLE
// advertisement (manufacturer company id 0x004C) whose bytes encode lid-open
// and in-ear status — the same advertisement the Android client parses.
//
// We use it for ONE decision only: "are the AirPods out of the case / nearby?"
// If they are sitting in a closed case, the handover logic must NOT attempt a
// BT connect on a media/call trigger, because that would poke the BT stack and
// (on success) hijack audio away from whatever device the user is actually on
// (Jabra/Dell headset, speakers, etc.).
//
// Availability is derived from the freshness of "out-of-case evidence"
// (lid open OR a pod in-ear). If no such evidence has been seen within
// kStaleWindowMs, the AirPods are treated as in-case / away.
class BleProximityWatcher {
public:
    BleProximityWatcher() = default;
    ~BleProximityWatcher();

    BleProximityWatcher(const BleProximityWatcher&) = delete;
    BleProximityWatcher& operator=(const BleProximityWatcher&) = delete;

    void start();
    void stop();

    // True if the AirPods appear to be out of the case / nearby — i.e. it is
    // reasonable to attempt a takeover. Fail-open: returns true if the watcher
    // never started or is still within its warm-up window, so a BLE problem can
    // never silently disable handover.
    bool airpodsAvailable() const;

private:
    void onReceived(
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher const&,
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs const& args);

    // Apple proximity-pairing advertisement: company id 0x004C, payload begins
    // 0x07 0x19 (type, length). Returns true and sets outOfCase if parsed.
    static bool parseProximity(const uint8_t* data, uint32_t len, bool& outOfCase);

    // 15 s mirrors Android's STALE_DEVICE_TIMEOUT_MS. AirPods advertise roughly
    // every 1-2 s while a lid is open / pods are in use, so 15 s comfortably
    // bridges sparse advertising without going stale during real use.
    static constexpr std::int64_t kStaleWindowMs = 15'000;
    // Warm-up grace after start(): fail-open so a freshly-launched app doesn't
    // suppress a legitimate takeover before the first beacon arrives.
    static constexpr std::int64_t kWarmupMs = 20'000;

    // RSSI proximity guard. AirPods broadcast BLE proximity adverts under a
    // rotating Resolvable Private Address (RPA), not their classic A2DP MAC, so
    // we cannot match the beacon to our configured MAC. Without the IRK (which
    // requires AACP — unavailable on Windows) we can't cryptographically verify
    // the RPA either. Instead we use signal strength as a proxy: our AirPods are
    // on the user's body next to the PC (≈ -45..-65 dBm); a neighbour's pair
    // through a wall is typically -75 dBm or weaker. Mirrors Android's no-IRK
    // RSSI_THRESHOLD_LIMITED_MODE path.
    static constexpr std::int16_t kRssiThresholdDbm = -75;

    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher m_watcher{nullptr};
    winrt::event_token m_receivedToken{};

    std::atomic<bool> m_running{false};
    // steady_clock milliseconds; 0 = never.
    std::atomic<std::int64_t> m_lastOutOfCaseMs{0};
    std::atomic<std::int64_t> m_startedAtMs{0};
};

} // namespace Propods
