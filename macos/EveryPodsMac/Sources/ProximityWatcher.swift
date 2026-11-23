// ProximityWatcher.swift
//
// Answers "are the AirPods out of their case and nearby?" from Apple's BLE proximity beacons, so a media or
// call trigger does not poke Bluetooth (and hijack another headset) while the AirPods are stowed. A port of
// windows/src/BleProximityWatcher: same parsing, same thresholds, and it fails open (returns true) whenever
// it cannot tell. Manual "Connect AirPods now" bypasses it.

import CoreBluetooth
import Foundation

final class ProximityWatcher: NSObject, CBCentralManagerDelegate {
    private static let appleCompanyID: UInt16 = 0x004C
    private static let rssiThreshold = -75         // ignore weak beacons (a neighbour's AirPods)
    private static let staleWindow: TimeInterval = 15
    private static let warmup: TimeInterval = 20

    private var manager: CBCentralManager?
    private var running = false
    private var startedAt = Date.distantPast
    private var lastInCase = Date.distantPast
    private var lastOutOfCase = Date.distantPast

    func start() {
        guard manager == nil else { return }
        startedAt = Date()
        manager = CBCentralManager(delegate: self, queue: .main)
    }

    func stop() {
        manager?.stopScan()
        manager = nil
        running = false
    }

    /// False only when the freshest beacon positively says "closed case, neither pod in an ear".
    var airPodsAvailable: Bool {
        guard running else { return true }
        if Date().timeIntervalSince(startedAt) < Self.warmup { return true }
        if lastInCase > lastOutOfCase && Date().timeIntervalSince(lastInCase) < Self.staleWindow { return false }
        return true
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            running = true
            Log.write("Proximity: scanning for AirPods beacons")
        } else {
            running = false
            Log.write("Proximity: Bluetooth scan unavailable (state \(central.state.rawValue)); fail open")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard RSSI.intValue >= Self.rssiThreshold, RSSI.intValue < 0,
              let data = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data, data.count >= 2 else { return }
        let company = UInt16(data[data.startIndex]) | UInt16(data[data.startIndex + 1]) << 8
        guard company == Self.appleCompanyID else { return }
        let bytes = [UInt8](data.dropFirst(2))
        guard let inCase = Self.parse(bytes) else { return }
        let changed = inCase ? lastInCase < lastOutOfCase : lastOutOfCase < lastInCase
        if inCase { lastInCase = Date() } else { lastOutOfCase = Date() }
        if changed { Log.write("Proximity: AirPods \(inCase ? "in case" : "out of case") (RSSI \(RSSI.intValue))") }
    }

    /// Apple proximity-pairing message (after the company id): [0]=0x07, [5]=status, [8]=lid. Returns nil
    /// for other Apple messages. "In case" = lid closed and neither pod in an ear.
    static func parse(_ bytes: [UInt8]) -> Bool? {
        guard bytes.count >= 9, bytes[0] == 0x07 else { return nil }
        let status = Int(bytes[5]), lid = Int(bytes[8])
        let primaryLeft = (status >> 5) & 1 == 1
        let thisInCase = (status >> 6) & 1 == 1
        let flip = primaryLeft != thisInCase
        let leftInEar = flip ? (status & 0x08) != 0 : (status & 0x02) != 0
        let rightInEar = flip ? (status & 0x02) != 0 : (status & 0x08) != 0
        let lidOpen = (lid >> 3) & 1 == 0
        return !lidOpen && !leftInEar && !rightInEar
    }
}
