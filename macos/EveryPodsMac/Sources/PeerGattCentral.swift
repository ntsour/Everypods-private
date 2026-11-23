// PeerGattCentral.swift
//
// Peer links over Bluetooth LE, central side (Everypods docs/peer-ble-transport.md). Finds phones that advertise
// the EveryPods peer GATT service, connects, subscribes to TX and exchanges records over RX/TX. CoreBluetooth has
// no channel numbers or SDP cache, so a phone whose app restarted is found again within seconds.
//
// The characteristics require encryption, which the existing Bluetooth pairing provides (no prompt on a bonded
// pair). A phone that does not have this Mac configured refuses the subscription; it is left alone for a minute.
//
// Reliability rules (each was hit in the spike or is handled the same way in bitchat's Apple client):
// - The phone app restarting under a live link: macOS keeps the LE link, so there is no disconnect. Handle
//   didModifyServices, and rediscover when a PING goes unanswered for 3 s.
// - Bluetooth off/resetting invalidates every CBPeripheral: drop all state without issuing commands.
// - A scan does not report a phone macOS is already connected to: poll retrieveConnectedPeripherals.
// - After a disconnect, reconnect to the same peripheral and keep scanning (its LE address may rotate).

import CoreBluetooth
import Foundation

final class PeerGattCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let serviceUUID = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A04")
    static let rxUUID = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A05")
    static let txUUID = CBUUID(string: "78FC4C78-0BDC-435D-B4A8-7D8B5F0C0A06")

    /// A ready link to one phone, identified by its HELLO.
    struct Peer: Equatable {
        let installationId: String
        let name: String
    }

    var onReady: ((Peer) -> Void)?
    var onPacket: ((Peer, Data) -> Void)?
    var onEnded: ((Peer) -> Void)?
    /// Records the central does not handle itself (SETUP_ACK, V2, unknown types), from any session with a HELLO.
    var onRecord: ((Peer, UInt8, Data) -> Void)?
    var localHello: () -> PeerGattRecords.Hello = {
        PeerGattRecords.Hello(installationId: UUID().uuidString.lowercased(), name: Host.current().localizedName ?? "Mac",
                              capabilities: ["legacy1"])
    }

    private let pingInterval: TimeInterval = 2
    private let staleAfter: TimeInterval = 3
    private let rediscoverWindow: TimeInterval = 5
    private let refusedRetry: TimeInterval = 60
    private let maxQueuedBytes = 64 * 1024

    private final class Session {
        let peripheral: CBPeripheral
        var rx: CBCharacteristic?
        var decoder = PeerGattRecords.Decoder()
        var writes: [Data] = []
        var queuedBytes = 0
        var subscribed = false
        var hello: PeerGattRecords.Hello?
        var ready = false
        var sequence: UInt32 = 0
        var pings: [UInt32: Date] = [:]
        var rediscoverDeadline: Date?
        var startedAt = Date()

        init(_ peripheral: CBPeripheral) { self.peripheral = peripheral }

        var peer: Peer? { hello.map { Peer(installationId: $0.installationId, name: $0.name) } }
        var label: String { hello?.name ?? String(peripheral.identifier.uuidString.prefix(8)) }

        func resetStream() {
            rx = nil
            decoder.reset()
            writes.removeAll()
            queuedBytes = 0
            subscribed = false
            hello = nil
            pings.removeAll()
        }
    }

    private var manager: CBCentralManager?
    private var sessions: [UUID: Session] = [:]
    private var refused: [UUID: Date] = [:]
    private var timer: Timer?

    /// Every phone with a live session and an exchanged HELLO, configured or not.
    var readyPeers: [Peer] { sessions.values.filter(\.ready).compactMap(\.peer) }

    /// Sends one record to the phone with this installation ID over any session with an exchanged HELLO (the phone
    /// need not be an authorized peer: this carries the setup exchange). False if there is no such session.
    @discardableResult
    func sendRecord(type: UInt8, payload: Data, to installationId: String) -> Bool {
        guard let session = sessions.values.first(where: { $0.ready && $0.hello?.installationId == installationId }),
              let record = PeerGattRecords.encode(type, payload) else { return false }
        return write(record, to: session)
    }

    func start() {
        guard manager == nil else { return }
        manager = CBCentralManager(delegate: self, queue: .main)
        timer = Timer.scheduledTimer(withTimeInterval: pingInterval, repeats: true) { [weak self] _ in self?.tick() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        if let manager, manager.state == .poweredOn {
            manager.stopScan()
            sessions.values.forEach { manager.cancelPeripheralConnection($0.peripheral) }
        }
        dropAll(reason: "stopped")
        manager?.delegate = nil
        manager = nil
    }

    /// Forgets which phones refused this Mac and tries them again now (after this Mac was authorized with one).
    func retryRefused() {
        refused.removeAll()
        attachToConnected()
        scan()  // restarting the scan reports advertising phones again
    }

    /// Sends one legacy packet to the phone with this installation ID; false if it has no ready link.
    @discardableResult
    func send(_ bytes: [UInt8], to installationId: String) -> Bool {
        guard let session = sessions.values.first(where: { $0.ready && $0.hello?.installationId == installationId }),
              let record = PeerGattRecords.encode(PeerGattRecords.legacy, Data(bytes)) else { return false }
        return write(record, to: session)
    }

    // MARK: Scanning and attaching

    private func scan() {
        guard let manager, manager.state == .poweredOn else { return }
        manager.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    /// Attaches to phones macOS is already connected to (a scan does not report them).
    private func attachToConnected() {
        guard let manager, manager.state == .poweredOn else { return }
        for peripheral in manager.retrieveConnectedPeripherals(withServices: [Self.serviceUUID]) {
            consider(peripheral, via: "already connected to this Mac")
        }
    }

    private func consider(_ peripheral: CBPeripheral, via: String) {
        guard let manager else { return }
        if let session = sessions[peripheral.identifier] {
            // Connected or connecting already, or a pending reconnect is armed.
            if session.peripheral.state != .disconnected { return }
        }
        if let since = refused[peripheral.identifier], Date().timeIntervalSince(since) < refusedRetry { return }
        let session = Session(peripheral)
        sessions[peripheral.identifier] = session
        peripheral.delegate = self
        Log.write("PeerGatt: connecting to \(session.label) (\(via))")
        manager.connect(peripheral, options: nil)
    }

    // MARK: Session state

    private func becomeReadyIfComplete(_ session: Session) {
        guard !session.ready, session.subscribed, let hello = session.hello, let peer = session.peer else { return }
        if sessions.values.contains(where: { $0 !== session && $0.ready && $0.hello?.installationId == hello.installationId }) {
            Log.write("PeerGatt: \(hello.name) is already linked; dropping the duplicate")
            return end(session, reason: "duplicate", reconnect: false)
        }
        session.ready = true
        session.rediscoverDeadline = nil
        Log.write(String(format: "PeerGatt[%@]: ready (%.1fs)", hello.name, Date().timeIntervalSince(session.startedAt)))
        onReady?(peer)
    }

    private func unready(_ session: Session, reason: String) {
        guard session.ready, let peer = session.peer else { return }
        session.ready = false
        Log.write("PeerGatt[\(peer.name)]: link down (\(reason))")
        onEnded?(peer)
    }

    /// The phone's service went away under a live link (its app restarted): find it again on the same connection,
    /// and reconnect only if that does not work within the window.
    private func rediscover(_ session: Session, reason: String) {
        guard session.peripheral.state == .connected else { return }
        if session.rediscoverDeadline == nil {
            Log.write("PeerGatt[\(session.label)]: \(reason); rediscovering on the same link")
            session.rediscoverDeadline = Date().addingTimeInterval(rediscoverWindow)
            session.startedAt = Date()
        }
        unready(session, reason: reason)
        session.resetStream()
        session.peripheral.discoverServices([Self.serviceUUID])
    }

    /// Ends a session. With `reconnect`, a pending connect is armed so the link returns when the phone does.
    private func end(_ session: Session, reason: String, reconnect: Bool) {
        unready(session, reason: reason)
        session.resetStream()
        session.rediscoverDeadline = nil
        guard let manager, manager.state == .poweredOn else { return }
        if reconnect {
            session.startedAt = Date()
            manager.connect(session.peripheral, options: nil)
        } else {
            manager.cancelPeripheralConnection(session.peripheral)
            sessions[session.peripheral.identifier] = nil
        }
    }

    private func dropAll(reason: String) {
        for session in sessions.values { unready(session, reason: reason) }
        sessions.removeAll()
    }

    private func tick() {
        guard let manager, manager.state == .poweredOn else { return }
        attachToConnected()
        let now = Date()
        for session in sessions.values {
            if let deadline = session.rediscoverDeadline, now >= deadline {
                Log.write("PeerGatt[\(session.label)]: rediscovery timed out; reconnecting")
                manager.cancelPeripheralConnection(session.peripheral)
                end(session, reason: "rediscovery timed out", reconnect: true)
                continue
            }
            guard session.subscribed, session.rx != nil else { continue }
            if let oldest = session.pings.values.min(), now.timeIntervalSince(oldest) >= staleAfter {
                rediscover(session, reason: "PING unanswered for \(Int(staleAfter)) s")
                continue
            }
            session.sequence &+= 1
            var value = session.sequence.bigEndian
            session.pings[session.sequence] = now
            if let record = PeerGattRecords.encode(PeerGattRecords.ping, Data(bytes: &value, count: 4)) {
                write(record, to: session)
            }
        }
    }

    // MARK: Writing

    @discardableResult
    private func write(_ record: Data, to session: Session) -> Bool {
        guard session.rx != nil else { return false }
        guard session.queuedBytes + record.count <= maxQueuedBytes else {
            Log.write("PeerGatt[\(session.label)]: write queue full; rediscovering")
            rediscover(session, reason: "write queue full")
            return false
        }
        let size = max(20, session.peripheral.maximumWriteValueLength(for: .withoutResponse))
        for chunk in PeerGattRecords.chunk(record, size: size) {
            session.writes.append(chunk)
            session.queuedBytes += chunk.count
        }
        drain(session)
        return true
    }

    private func drain(_ session: Session) {
        guard let rx = session.rx else { return }
        while !session.writes.isEmpty, session.peripheral.canSendWriteWithoutResponse {
            let chunk = session.writes.removeFirst()
            session.queuedBytes -= chunk.count
            session.peripheral.writeValue(chunk, for: rx, type: .withoutResponse)
        }
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            Log.write("PeerGatt: Bluetooth on; scanning")
            scan()
            attachToConnected()
        default:
            // Every CBPeripheral is invalid now, and commands would be rejected: retire state locally.
            if !sessions.isEmpty { Log.write("PeerGatt: Bluetooth state \(central.state.rawValue); dropping links") }
            dropAll(reason: "Bluetooth state \(central.state.rawValue)")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        consider(peripheral, via: "advertising, rssi \(RSSI)")
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard let session = sessions[peripheral.identifier] else { return central.cancelPeripheralConnection(peripheral) }
        session.resetStream()
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard let session = sessions[peripheral.identifier] else { return }
        Log.write("PeerGatt[\(session.label)]: connect failed (\(error?.localizedDescription ?? "unknown")); retrying")
        end(session, reason: "connect failed", reconnect: true)
        scan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard let session = sessions[peripheral.identifier] else { return }
        if session.ready || error != nil {
            Log.write("PeerGatt[\(session.label)]: disconnected (\(error?.localizedDescription ?? "no error"))")
        }
        end(session, reason: "disconnected", reconnect: true)
        // Restart the scan for fresh discovery events (the phone's LE address may have rotated).
        central.stopScan()
        scan()
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        guard let session = sessions[peripheral.identifier] else { return }
        let ours = invalidatedServices.contains { $0.uuid == Self.serviceUUID } ||
            !(peripheral.services ?? []).contains { $0.uuid == Self.serviceUUID }
        if ours { rediscover(session, reason: "services changed on the phone") }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let session = sessions[peripheral.identifier] else { return }
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            if session.rediscoverDeadline != nil {
                // The phone app is still starting: look again shortly, within the rediscovery window.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.sessions[peripheral.identifier] === session, session.rediscoverDeadline != nil,
                          peripheral.state == .connected else { return }
                    peripheral.discoverServices([Self.serviceUUID])
                }
            } else {
                rediscover(session, reason: "service missing")
            }
            return
        }
        peripheral.discoverCharacteristics([Self.rxUUID, Self.txUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let session = sessions[peripheral.identifier] else { return }
        let characteristics = service.characteristics ?? []
        guard let rx = characteristics.first(where: { $0.uuid == Self.rxUUID }),
              let tx = characteristics.first(where: { $0.uuid == Self.txUUID }) else {
            return rediscover(session, reason: "characteristics missing")
        }
        session.rx = rx
        peripheral.setNotifyValue(true, for: tx)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.txUUID, let session = sessions[peripheral.identifier] else { return }
        if let error {
            // Not bonded, or the phone does not have this Mac configured: leave it alone for a while.
            Log.write("PeerGatt[\(session.label)]: subscription refused (\(error.localizedDescription)); retry in \(Int(refusedRetry)) s")
            refused[peripheral.identifier] = Date()
            return end(session, reason: "refused", reconnect: false)
        }
        guard characteristic.isNotifying else { return }
        session.subscribed = true
        if let hello = PeerGattRecords.encodeHello(localHello()) { write(hello, to: session) }
        becomeReadyIfComplete(session)
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        if let session = sessions[peripheral.identifier] { drain(session) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.txUUID, let session = sessions[peripheral.identifier],
              let data = characteristic.value else { return }
        let records: [PeerGattRecords.Record]
        do { records = try session.decoder.feed(data) } catch {
            return rediscover(session, reason: "bad stream (\(error))")
        }
        for record in records {
            switch record.type {
            case PeerGattRecords.hello:
                guard let hello = try? PeerGattRecords.parseHello(record.payload) else {
                    return rediscover(session, reason: "bad HELLO")
                }
                session.hello = hello
                becomeReadyIfComplete(session)
            case PeerGattRecords.legacy:
                if session.ready, let peer = session.peer { onPacket?(peer, record.payload) }
            case PeerGattRecords.pong:
                if record.payload.count == 4 {
                    let seq = record.payload.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).bigEndian }
                    session.pings[seq] = nil
                }
            default:
                // SETUP_ACK, and V2 (not spoken by the Mac yet) or unknown types, are for the owner to handle or skip.
                if session.ready, let peer = session.peer { onRecord?(peer, record.type, record.payload) }
            }
        }
    }
}
