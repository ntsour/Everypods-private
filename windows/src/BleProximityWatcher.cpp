#include "BleProximityWatcher.hpp"

#include "Logger.hpp"
#include "ProximityRules.hpp"

#include <chrono>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

namespace Propods {

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using winrt::Windows::Storage::Streams::DataReader;

namespace {

constexpr std::uint16_t kAppleCompanyId = 0x004C;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

BleProximityWatcher::~BleProximityWatcher() {
    stop();
}

void BleProximityWatcher::start() {
    if (m_running.exchange(true)) return;

    try {
        m_watcher = BluetoothLEAdvertisementWatcher();
        // Active scanning is required to receive the full manufacturer-data
        // payload (the proximity message) rather than just the scan-response header.
        m_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        m_receivedToken = m_watcher.Received({this, &BleProximityWatcher::onReceived});
        m_startedAtMs.store(nowMs());
        m_watcher.Start();
        log::info("BleProximityWatcher: started (Apple 0x004C proximity scan, {}s out-of-case window)",
                  kOutOfCaseWindowMs / 1000);
    } catch (const winrt::hresult_error& e) {
        // Fail-open: leave m_running false so airpodsAvailable() returns true and
        // handover behaves exactly as before this feature existed.
        m_running.store(false);
        log::warn("BleProximityWatcher: failed to start BLE scan: 0x{:08X} {} — "
                  "in-case detection disabled (handover proceeds as before)",
                  (std::uint32_t)e.code().value, winrt::to_string(e.message()));
    }
}

void BleProximityWatcher::stop() {
    if (!m_running.exchange(false)) return;
    try {
        if (m_watcher) {
            m_watcher.Stop();
            if (m_receivedToken) m_watcher.Received(m_receivedToken);
        }
    } catch (...) {}
    m_watcher = nullptr;
    m_loggedFirstBeacon.store(false);
    log::info("BleProximityWatcher: stopped");
}

void BleProximityWatcher::setPaused(bool paused) {
    std::scoped_lock lk{m_toggleMutex};
    if (paused == m_paused) return;
    m_paused = paused;
    if (paused) {
        log::info("BleProximityWatcher: pausing scan while this PC holds the AirPods");
        stop();
    } else {
        log::info("BleProximityWatcher: resuming scan");
        start();
    }
}

bool BleProximityWatcher::airpodsAvailable() const {
    // Fail-open when the watcher isn't running (BLE unsupported / failed to start).
    if (!m_running.load()) return true;

    const std::int64_t now = nowMs();

    // Warm-up grace: don't suppress a takeover before the first beacon can arrive.
    const std::int64_t startedAt = m_startedAtMs.load();
    if (startedAt != 0 && now - startedAt < kWarmupMs) return true;

    // Block ONLY when the latest beacon said in case and no out-of-case beacon
    // came within kOutOfCaseWindowMs (see ProximityRules.hpp). A missing beacon
    // must never block a legitimate handover of pods in use on a peer.
    return airpodsAvailableFor(now, m_lastInCaseMs.load(), m_lastOutOfCaseMs.load());
}

bool BleProximityWatcher::parseProximity(const uint8_t* data, uint32_t len, bool& inCase) {
    return parseProximityBeacon(data, len, inCase);
}

void BleProximityWatcher::onReceived(
    BluetoothLEAdvertisementWatcher const&,
    BluetoothLEAdvertisementReceivedEventArgs const& args) {
    try {
        // Proximity guard: ignore weak beacons (a neighbour's AirPods through a
        // wall) since we can't match the rotating RPA to our MAC. See header.
        const std::int16_t rssi = args.RawSignalStrengthInDBm();
        if (rssi < kRssiThresholdDbm) return;

        auto mfdList = args.Advertisement().ManufacturerData();
        for (auto&& mfd : mfdList) {
            if (mfd.CompanyId() != kAppleCompanyId) continue;

            auto buffer = mfd.Data();
            const uint32_t len = buffer.Length();
            if (len == 0) continue;

            std::vector<uint8_t> bytes(len);
            auto reader = DataReader::FromBuffer(buffer);
            reader.ReadBytes(bytes);

            bool inCase = false;
            if (!parseProximity(bytes.data(), len, inCase)) continue;

            if (!m_loggedFirstBeacon.exchange(true))
                log::info("BleProximityWatcher: first beacon — AirPods {} (RSSI {} dBm)",
                          inCase ? "in case" : "out of case", rssi);

            // Logged at debug to avoid flooding (beacons arrive ~1/s).
            if (inCase) {
                m_lastInCaseMs.store(nowMs());
                log::debug("BleProximityWatcher: AirPods proximity beacon — IN CASE (RSSI {} dBm)", rssi);
            } else {
                m_lastOutOfCaseMs.store(nowMs());
                log::debug("BleProximityWatcher: AirPods proximity beacon — out of case (RSSI {} dBm)", rssi);
            }
            return;  // handled the Apple entry
        }
    } catch (...) {
        // Never let a malformed advertisement crash the scan callback.
    }
}

} // namespace Propods
