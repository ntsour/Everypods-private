#include "BleProximityWatcher.hpp"

#include "Logger.hpp"

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
        log::info("BleProximityWatcher: started (Apple 0x004C proximity scan, {}s availability window)",
                  kStaleWindowMs / 1000);
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
    log::info("BleProximityWatcher: stopped");
}

bool BleProximityWatcher::airpodsAvailable() const {
    // Fail-open when the watcher isn't running (BLE unsupported / failed to start).
    if (!m_running.load()) return true;

    const std::int64_t now = nowMs();

    // Warm-up grace: don't suppress a takeover before the first beacon can arrive.
    const std::int64_t startedAt = m_startedAtMs.load();
    if (startedAt != 0 && now - startedAt < kWarmupMs) return true;

    const std::int64_t inCase  = m_lastInCaseMs.load();
    const std::int64_t outCase = m_lastOutOfCaseMs.load();

    // Block ONLY when the most recent fresh observation is positively in-case.
    // Otherwise (out-of-case latest, or no beacons, or stale) allow — a connect()
    // to cased pods fails harmlessly, while a missing beacon must never block a
    // legitimate handover of pods that are actually in use on a peer.
    if (inCase > outCase && (now - inCase) < kStaleWindowMs)
        return false;
    return true;
}

bool BleProximityWatcher::parseProximity(const uint8_t* data, uint32_t len, bool& inCase) {
    // Apple proximity-pairing message layout (manufacturer data after company id):
    //   data[0]   = 0x07 (type: proximity pairing / "nearby")
    //   data[1]   = 0x19 (length, 25)
    //   data[5]   = status byte (primary-pod + in-case + in-ear bits)
    //   data[8]   = lid state (bit 3: 0 = open, 1 = closed)
    // We only need indices up to 8.
    if (len < 9 || data[0] != 0x07) return false;

    const int status = data[5];
    const int lid     = data[8];

    // In-ear detection (ported verbatim from Android BLEManager.parseProximityMessage):
    const bool primaryLeft = ((status >> 5) & 0x01) == 1;
    const bool thisInCase  = ((status >> 6) & 0x01) == 1;
    const bool xorFactor   = primaryLeft ^ thisInCase;
    const bool leftInEar   = xorFactor ? (status & 0x08) != 0 : (status & 0x02) != 0;
    const bool rightInEar  = xorFactor ? (status & 0x02) != 0 : (status & 0x08) != 0;

    const bool lidOpen = ((lid >> 3) & 0x01) == 0;

    // "In case" = lid closed AND neither pod in an ear, i.e. both pods stowed in
    // a closed case. This is the only state in which we suppress a takeover.
    // Pods in ears (case closed in a pocket) report in-ear → NOT in-case → we
    // still allow the handover. Anything ambiguous defaults to NOT in-case.
    inCase = !lidOpen && !leftInEar && !rightInEar;
    return true;
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
