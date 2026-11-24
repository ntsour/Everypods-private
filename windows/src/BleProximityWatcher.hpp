#pragma once

#include <atomic>
#include <mutex>
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

    // Pause scanning while this PC holds the AirPods: the result only gates
    // taking them, and a continuous active scan competes for the radio with
    // the AirPods stream and the phone link. On 2026-11-20 the Pixel-PC link
    // timed out (HCI 0x08) about every 4-5 min only while the PC held the
    // AirPods. While paused, airpodsAvailable() fails open (returns true), and
    // resuming restarts the warm-up grace. Safe to call from any thread.
    void setPaused(bool paused);

    // True if it is reasonable to attempt a takeover. The gate is deliberately
    // conservative: it returns false ONLY when we have positive, fresh evidence
    // the AirPods are physically in the case (lid closed AND neither pod in an
    // ear). Anything else — out-of-case beacons, no beacons at all, BLE
    // unavailable, warm-up — returns true so a legitimate handover is never
    // blocked. (A connect() to truly-cased pods just fails harmlessly; audio is
    // only hijacked when the pods are actually reachable, which is exactly when
    // we DO want the handover.)
    bool airpodsAvailable() const;

    // Fresh evidence a bud is in an ear: a beacon saying so within kWornWindowMs.
    // Gates the playback hold and its panel (Android's airPodsLikelyWorn rule):
    // a phone reports "held" while its Bluetooth profile is up, which stays true
    // with the AirPods in their case. Unknown (no beacon, warm-up, scan paused)
    // counts as not worn; only a watcher that could not start fails open.
    bool budLikelyWorn() const;

private:
    void onReceived(
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher const&,
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs const& args);

    // Apple proximity-pairing advertisement: company id 0x004C, payload begins
    // 0x07 0x19 (type, length). Returns true if parsed; sets inCase = lid closed
    // AND neither pod in an ear (i.e. both pods stowed in a closed case).
    // worn = at least one pod in an ear.
    static bool parseProximity(const uint8_t* data, uint32_t len, bool& inCase, bool& worn);

    // 15 s mirrors Android's STALE_DEVICE_TIMEOUT_MS. AirPods advertise roughly
    // every 1-2 s while a lid is open / pods are in use, so 15 s comfortably
    // bridges sparse advertising without going stale during real use.
    static constexpr std::int64_t kStaleWindowMs = 15'000;
    // Warm-up grace after start(): fail-open so a freshly-launched app doesn't
    // suppress a legitimate takeover before the first beacon arrives.
    static constexpr std::int64_t kWarmupMs = 20'000;
    // Out of their case AirPods advertise every 3-9 s (measured on the Mac
    // 2026-11-23), so 20 s rides out gaps.
    static constexpr std::int64_t kWornWindowMs = 20'000;

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
    std::mutex m_toggleMutex;   // serialises setPaused() start/stop
    bool m_paused = false;      // guarded by m_toggleMutex
    // steady_clock milliseconds; 0 = never. We track the latest in-case and
    // out-of-case observations separately; the gate blocks only when the most
    // recent fresh reading is in-case.
    std::atomic<std::int64_t> m_lastInCaseMs{0};
    std::atomic<std::int64_t> m_lastOutOfCaseMs{0};
    std::atomic<std::int64_t> m_lastWornMs{0};
    std::atomic<bool> m_startFailed{false};
    std::atomic<std::int64_t> m_startedAtMs{0};
};

} // namespace Propods
