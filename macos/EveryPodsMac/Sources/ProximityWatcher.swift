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

    /// Called on the main queue when a case lid opens nearby (see `lidOpeningStarts`).
    var onLidOpened: (() -> Void)?
    /// An open case advertises every second or so: a lid-open beacon after this long without one is a new opening.
    static let lidOpenRearm: TimeInterval = 60
    /// With no lid-closed beacon seen, the first lid-open beacon of a scan counts only after this much scanning:
    /// a shut case goes quiet, so the silence is the evidence it was shut.
    static let lidQuiet: TimeInterval = 10
    private var lidScanStartedAt = Date()
    private var lastLidOpen: Date?
    private var lastLidClosed: Date?

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
            // Lid evidence from an earlier scan (before sleep) says nothing about now.
            lidScanStartedAt = Date()
            lastLidOpen = nil
            lastLidClosed = nil
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
        if let open = Self.lidOpen(bytes) {
            let now = Date()
            if open {
                let starts = Self.lidOpeningStarts(now: now, scanStart: lidScanStartedAt, lastOpen: lastLidOpen,
                                                   lastClosed: lastLidClosed)
                lastLidOpen = now
                if starts {
                    Log.write("Proximity: case lid opened (RSSI \(RSSI.intValue))")
                    onLidOpened?()
                }
            } else {
                lastLidClosed = now
            }
        }
        guard let inCase = Self.parse(bytes) else { return }
        if inCase { lastInCase = Date() } else { lastOutOfCase = Date() }
        if lastState != inCase {
            lastState = inCase
            Log.write("Proximity: AirPods \(inCase ? "in case" : "out of case") (RSSI \(RSSI.intValue))") }
    }

    /// The lid state from an AirPods proximity-pairing beacon (after the company id): [0]=0x07, [1]=length 0x19,
    /// [2]=0x01, [8] bit 3 set = lid closed. Nil for any other message, including a shorter type-0x07 message from
    /// other Apple devices whose byte 8 is 0xff. Also nil while either bud is in an ear: worn buds can report the
    /// lid open (byte 8 = 0x11 captured with both buds in, 2026-11-23), which would let AirPods worn on a device
    /// outside EveryPods count as an opening. Opening the case with both buds inside is never worn.
    static func lidOpen(_ bytes: [UInt8]) -> Bool? {
        guard bytes.count >= 9, bytes[0] == 0x07, bytes[1] == 0x19, bytes[2] == 0x01 else { return nil }
        // Status bits 0x02 and 0x08 are the two buds' in-ear flags (which is left depends on the primary bud).
        if bytes[5] & 0x0A != 0 { return nil }
        return (bytes[8] >> 3) & 1 == 0
    }

    /// A lid-open beacon starts a new opening when the lid must have been shut first:
    /// 1. a lid-closed beacon came after the last lid-open one (or since the scan started);
    /// 2. else, for the first lid-open beacon of the scan, the scan has run `lidQuiet` without beacons (a shut case
    ///    is silent). Without this, an open case or a dropped connection counted as an opening as soon as the
    ///    scan saw the case (0.6 s after a disconnect on Windows);
    /// 3. else, `lidOpenRearm` passed without a lid-open beacon.
    /// Times are per scan (cleared when it starts); beacons from worn buds never update them.
    static func lidOpeningStarts(now: Date, scanStart: Date, lastOpen: Date?, lastClosed: Date?) -> Bool {
        if let lastClosed, lastClosed > (lastOpen ?? .distantPast) { return true }
        guard let lastOpen else { return now.timeIntervalSince(scanStart) >= lidQuiet }
        return now.timeIntervalSince(lastOpen) >= lidOpenRearm
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
