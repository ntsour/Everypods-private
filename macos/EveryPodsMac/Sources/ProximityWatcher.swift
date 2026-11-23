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
    /// An out-of-case beacon this recent keeps the AirPods available. Out of their case, AirPods advertise every
    /// 3-9 s (measured 2026-11-23), so this rides out gaps and any other AirPods' in-case beacons nearby.
    private static let outOfCaseWindow: TimeInterval = 20
    private static let warmup: TimeInterval = 20

    private var manager: CBCentralManager?
    private var running = false
    private var startedAt = Date.distantPast
    private var lastInCase = Date.distantPast
    private var lastOutOfCase = Date.distantPast
    private var lastState: Bool?

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

    /// False only when the latest beacon said "closed case, neither pod in an ear" and no out-of-case beacon came
    /// within `outOfCaseWindow`. The in-case reading doesn't expire: a closed case stops advertising soon after
    /// it shuts, while opening it or taking a pod out starts out-of-case beacons at once.
    var airPodsAvailable: Bool {
        guard running else { return true }
        if Date().timeIntervalSince(startedAt) < Self.warmup { return true }
        let latestSaysInCase = lastInCase > lastOutOfCase
        let recentlyOut = Date().timeIntervalSince(lastOutOfCase) < Self.outOfCaseWindow
        return !latestSaysInCase || recentlyOut
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
        if inCase { lastInCase = Date() } else { lastOutOfCase = Date() }
        if lastState != inCase {
            lastState = inCase
            Log.write("Proximity: AirPods \(inCase ? "in case" : "out of case") (RSSI \(RSSI.intValue))") }
    }

    /// Apple proximity-pairing message (after the company id): [0]=0x07, [1]=length 0x19, [2]=0x01, [3-4]=model,
    /// [5]=status, [8]=lid. Returns nil for other Apple messages. "In case" = lid closed and neither pod in an ear.
    /// Type 0x07 is shared by a shorter message (length 0x11, [2]=0x06; seen nearby at a strong signal every 6 s)
    /// whose byte 8 is 0xff, which reads as "lid closed": accepting it made the state flip several times a second.
    static func parse(_ bytes: [UInt8]) -> Bool? {
        guard bytes.count >= 9, bytes[0] == 0x07, bytes[1] == 0x19, bytes[2] == 0x01 else { return nil }
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
