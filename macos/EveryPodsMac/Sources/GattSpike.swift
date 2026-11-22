// GattSpike.swift
//
// SPIKE (Everypods docs/peer-ble-transport.md §7 step 1): a CoreBluetooth central that finds the phone's
// EveryPods peer GATT service, connects, subscribes to TX and writes a ping to RX every 10 s; the phone echoes
// it back. No protocol, no security. Logs "GattSpike: ready" when the link can carry data, so restart trials can
// time the relink. Not for release.

import CoreBluetooth
import Foundation

final class GattSpike: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let service = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A04")
    private let rxUUID = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A05")
    private let txUUID = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A06")
    private let pingInterval: TimeInterval = 2
    /// A ping unanswered this long means the phone's GATT service went away under a link macOS kept up.
    private let staleAfter: TimeInterval = 3
    private var rediscoverDeadline: Timer?

    private var manager: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var pingTimer: Timer?
    private var sequence: UInt32 = 0
    private var sentAt: [UInt32: Date] = [:]
    private var echoes = 0
    private var lostAt: Date?
    private var discoveredAt: Date?

    func start() {
        manager = CBCentralManager(delegate: self, queue: .main)
    }

    private var attachTimer: Timer?

    private func scan() {
        guard let manager, manager.state == .poweredOn else { return }
        manager.scanForPeripherals(withServices: [service], options: nil)
        Log.write("GattSpike: scanning")
        // macOS may already hold an LE link to the phone (its own, or one CoreBluetooth restored when the phone
        // came back in range); a scan does not report a phone it is connected to. Attach to such a link directly.
        attachTimer?.invalidate()
        attachTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.attachToConnected() }
        attachToConnected()
    }

    private func attachToConnected() {
        guard let manager, manager.state == .poweredOn else { return }
        if let current = peripheral, current.state == .connected || current.state == .connecting, rx != nil || current.state == .connecting {
            attachTimer?.invalidate()
            attachTimer = nil
            return
        }
        guard let phone = manager.retrieveConnectedPeripherals(withServices: [service]).first else { return }
        if let current = peripheral, current !== phone { manager.cancelPeripheralConnection(current) }
        attachTimer?.invalidate()
        attachTimer = nil
        manager.stopScan()
        discoveredAt = Date()
        Log.write("GattSpike: phone already connected to this Mac; attaching (lost \(since(lostAt)) ago)")
        peripheral = phone
        phone.delegate = self
        manager.connect(phone, options: nil)  // completes at once for a connected peripheral
    }

    private func reset(_ reason: String, alreadyDisconnected: Bool = false) {
        pingTimer?.invalidate()
        pingTimer = nil
        rediscoverDeadline?.invalidate()
        rediscoverDeadline = nil
        rx = nil
        sentAt.removeAll()
        lostAt = lostAt ?? Date()
        Log.write("GattSpike: link lost (\(reason))")
        if let peripheral {
            if !alreadyDisconnected { manager?.cancelPeripheralConnection(peripheral) }
            // Reconnect to the same phone right away (a pending connect completes as soon as it is reachable),
            // and scan in case its LE address rotated.
            discoveredAt = Date()
            manager?.connect(peripheral, options: nil)
        }
        scan()
    }

    /// The service vanished under a live link (phone app restarted): find it again on the same connection, and
    /// reconnect only if that does not work quickly.
    private func rediscover(_ reason: String) {
        guard let peripheral, rediscoverDeadline == nil else { return }
        pingTimer?.invalidate()
        pingTimer = nil
        rx = nil
        sentAt.removeAll()
        lostAt = lostAt ?? Date()
        discoveredAt = Date()
        Log.write("GattSpike: \(reason); rediscovering on the same link")
        peripheral.discoverServices([service])
        rediscoverDeadline = Timer.scheduledTimer(withTimeInterval: 5, repeats: false) { [weak self] _ in
            self?.rediscoverDeadline = nil
            self?.reset("rediscovery timed out")
        }
    }

    private func since(_ date: Date?) -> String {
        guard let date else { return "?" }
        return String(format: "%.1fs", Date().timeIntervalSince(date))
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Log.write("GattSpike: Bluetooth state \(central.state.rawValue)")
        if central.state == .poweredOn { return scan() }
        guard peripheral != nil else { return }
        // CoreBluetooth invalidates every peripheral object when Bluetooth goes off: drop ours.
        reset("Bluetooth state \(central.state.rawValue)", alreadyDisconnected: true)
        peripheral = nil
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // Ignore a sighting only while our link is really up or still being set up. After a Bluetooth power
        // cycle the old CBPeripheral is dead (state .disconnected) even though the phone's identifier matches.
        if let current = self.peripheral, current.state == .connected || current.state == .connecting {
            return
        }
        if let current = self.peripheral { central.cancelPeripheralConnection(current) }
        let token = (advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data])?[service]
        discoveredAt = Date()
        Log.write("GattSpike: found \(peripheral.identifier.uuidString.prefix(8)) rssi \(RSSI) token \(token?.map { String(format: "%02x", $0) }.joined() ?? "none") (lost \(since(lostAt)) ago)")
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Log.write("GattSpike: connected (\(since(discoveredAt)) after found), MTU \(peripheral.maximumWriteValueLength(for: .withoutResponse) + 3)")
        peripheral.discoverServices([service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        reset("connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard peripheral === self.peripheral else { return }
        reset("disconnected: \(error?.localizedDescription ?? "no error")", alreadyDisconnected: true)
    }

    // MARK: CBPeripheralDelegate

    /// The phone's app restarted (its GATT database changed) while macOS kept the LE link up: our handles are
    /// gone, so discover the service again on the same connection instead of waiting for pings to fail.
    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        guard peripheral === self.peripheral else { return }
        rediscover("services changed on the phone (\(invalidatedServices.count) invalidated)")
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == service }) else {
            if rediscoverDeadline != nil {
                // The phone app is still starting: look again shortly, within the rediscovery window.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.rediscoverDeadline != nil, peripheral === self.peripheral else { return }
                    peripheral.discoverServices([self.service])
                }
                return
            }
            return rediscover("service missing: \(error?.localizedDescription ?? "not found")")
        }
        peripheral.discoverCharacteristics([rxUUID, txUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let chars = service.characteristics ?? []
        guard let rx = chars.first(where: { $0.uuid == rxUUID }), let tx = chars.first(where: { $0.uuid == txUUID }) else {
            return reset("characteristics missing")
        }
        self.rx = rx
        peripheral.setNotifyValue(true, for: tx)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == txUUID else { return }
        guard characteristic.isNotifying, error == nil else {
            return reset("subscribe failed: \(error?.localizedDescription ?? "not notifying")")
        }
        rediscoverDeadline?.invalidate()
        rediscoverDeadline = nil
        attachTimer?.invalidate()
        attachTimer = nil
        Log.write("GattSpike: ready (\(since(discoveredAt)) after found, \(since(lostAt)) after link lost)")
        lostAt = nil
        echoes = 0
        ping()
        pingTimer = Timer.scheduledTimer(withTimeInterval: pingInterval, repeats: true) { [weak self] _ in self?.ping() }
    }

    private func ping() {
        guard let peripheral, let rx else { return }
        if let oldest = sentAt.values.min(), Date().timeIntervalSince(oldest) >= staleAfter {
            return rediscover("ping unanswered for \(Int(staleAfter)) s")
        }
        sequence &+= 1
        var value = sequence.bigEndian
        sentAt[sequence] = Date()
        peripheral.writeValue(Data(bytes: &value, count: 4), for: rx, type: .withoutResponse)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == txUUID, let data = characteristic.value, data.count == 4 else { return }
        let seq = data.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
        guard let sent = sentAt.removeValue(forKey: seq) else { return }
        echoes += 1
        if echoes == 1 || echoes % 150 == 0 {
            Log.write(String(format: "GattSpike: echo %u rtt %.0f ms (%d echoes on this link)", seq, Date().timeIntervalSince(sent) * 1000, echoes))
        }
    }
}
