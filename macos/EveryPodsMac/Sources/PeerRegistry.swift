// PeerRegistry.swift
//
// The peers (phones and other computers) this Mac coordinates with, and their links. Every link is Bluetooth LE
// (PeerGattCentral, Everypods docs/peer-ble-transport.md): peers advertise the EveryPods GATT service and the Mac
// connects. There is no RFCOMM: dialling over classic Bluetooth paged absent peers and held the radio, which
// delayed connecting the AirPods by up to a minute, and macOS cannot keep RFCOMM channel numbers fresh.
// Packets are broadcast to every linked peer.

import Foundation
import IOBluetooth

final class PeerRegistry {
    struct Info {
        let address: String
        let name: String
        let connected: Bool
    }

    /// A phone reachable over Bluetooth LE that is not (yet) a configured peer.
    struct Candidate {
        let installationId: String
        let name: String
        /// The paired classic address, when the name matches exactly one paired device that is not a configured peer.
        let address: String?
    }

    var onPacket: ((Data) -> Void)?
    /// SETUP_ACK payloads: (installation ID, JSON).
    var onSetupAck: ((String, Data) -> Void)?
    /// `connected` is the state of the peer that changed.
    var onConnectionChange: ((String, Bool) -> Void)?

    /// Configured peers by address, in the order they were added.
    private var addresses: [String] = []
    private var started = false
    private let gatt = PeerGattCentral()
    /// Ready links: installation ID → configured peer address.
    private var gattLinks: [String: String] = [:]
    /// Phones with a session (HELLO exchanged) that match no configured peer: installation ID → phone. They stay
    /// connected and idle (kept alive by PING) so the Mac can be authorized over them.
    private var unmatched: [String: PeerGattCentral.Peer] = [:]

    var isAnyConnected: Bool { !gattLinks.isEmpty }
    var isEmpty: Bool { addresses.isEmpty }

    var infos: [Info] {
        addresses.map { Info(address: $0, name: Self.name(of: $0), connected: isReachable($0)) }
    }

    private static func name(of address: String) -> String {
        IOBluetoothDevice(addressString: normalizedAddress(address))?.name ?? address
    }

    private func isReachable(_ address: String) -> Bool {
        gattLinks.values.contains { sameAddress($0, address) }
    }

    /// Starts linking with the saved peers.
    func start() {
        started = true
        for address in Settings.peers where !addresses.contains(where: { sameAddress($0, address) }) {
            addresses.append(address)
        }
        gatt.localHello = {
            let name = IOBluetoothHostController.default()?.nameAsString() ?? Host.current().localizedName ?? "Mac"
            return PeerGattRecords.Hello(installationId: Settings.installationId, name: name, capabilities: ["legacy1"])
        }
        gatt.onReady = { [weak self] peer in self?.gattReady(peer) }
        gatt.onEnded = { [weak self] peer in self?.gattEnded(peer) }
        gatt.onRecord = { [weak self] peer, type, payload in
            if type == PeerGattRecords.setupAck { self?.onSetupAck?(peer.installationId, payload) }
        }
        gatt.onPacket = { [weak self] peer, data in
            guard let self, self.gattLinks[peer.installationId] != nil else { return }
            self.onPacket?(data)
        }
        gatt.start()
    }

    func stop() {
        started = false
        gatt.stop()
        let linked = Array(gattLinks.values)
        gattLinks.removeAll()
        unmatched.removeAll()
        linked.forEach { onConnectionChange?($0, false) }
    }

    /// Rebuilds the links (after wake, or when the Bluetooth stack was reset).
    func restart() {
        guard started else { return }
        stop()
        start()
    }

    /// A phone just authorized this Mac: retry the phones that refused it, without dropping the other links.
    func retryRefused() {
        rematch()
        gatt.retryRefused()
    }

    /// Tries again to match the phones that were not used (a peer was added or remembered since).
    func rematch() {
        for peer in Array(unmatched.values) { gattReady(peer) }
    }

    /// Phones reachable over Bluetooth LE that are not configured peers, for "Add peer…".
    var discoverable: [Candidate] {
        let paired = Settings.pairedDevices()
        return unmatched.values.sorted { $0.name < $1.name }.map { peer in
            let devices = paired.filter { Self.sameName($0.name ?? "", peer.name) }
            var address: String?
            if devices.count == 1, let found = devices[0].addressString,
               !addresses.contains(where: { sameAddress($0, found) }) {
                address = found
            }
            return Candidate(installationId: peer.installationId, name: peer.name, address: address)
        }
    }

    /// The installation ID of the phone with a live LE session for this configured peer, if any.
    func leInstallationId(for address: String) -> String? {
        let live = Set(gatt.readyPeers.map(\.installationId))
        if let id = Settings.gattPeerAddresses.first(where: { live.contains($0.key) && sameAddress($0.value, address) })?.key {
            return id
        }
        let name = Self.name(of: address)
        let named = gatt.readyPeers.filter { Self.sameName($0.name, name) }
        return named.count == 1 ? named[0].installationId : nil
    }

    func hasLESession(_ installationId: String) -> Bool {
        gatt.readyPeers.contains { $0.installationId == installationId }
    }

    /// Sends a setup record over the LE session with this phone (it need not be a configured peer).
    @discardableResult
    func sendSetupRecord(_ payload: Data, to installationId: String) -> Bool {
        gatt.sendRecord(type: PeerGattRecords.setupRequest, payload: payload, to: installationId)
    }

    /// Remembers that the phone with this installation ID is configured under `address`.
    func remember(installationId: String, address: String) {
        var known = Settings.gattPeerAddresses
        known[installationId] = address
        Settings.gattPeerAddresses = known
    }

    private static func sameName(_ a: String, _ b: String) -> Bool {
        a.trimmingCharacters(in: .whitespaces).caseInsensitiveCompare(b.trimmingCharacters(in: .whitespaces)) == .orderedSame
    }

    func add(_ address: String, persist: Bool = true) {
        guard !addresses.contains(where: { sameAddress($0, address) }) else { return }
        addresses.append(address)
        if persist { Settings.peers = Settings.peers + [address] }
        Log.write("Peers: added \(Self.name(of: address))")
    }

    func remove(_ address: String) {
        guard let index = addresses.firstIndex(where: { sameAddress($0, address) }) else { return }
        addresses.remove(at: index)
        Settings.peers = Settings.peers.filter { !sameAddress($0, address) }
        for (id, linked) in gattLinks where sameAddress(linked, address) { gattLinks[id] = nil }
        Settings.gattPeerAddresses = Settings.gattPeerAddresses.filter { !sameAddress($0.value, address) }
        Log.write("Peers: removed \(Self.name(of: address))")
        onConnectionChange?(address, false)
    }

    /// Sends to every linked peer; returns true if at least one accepted the packet.
    @discardableResult
    func send(_ bytes: [UInt8]) -> Bool {
        var sent = false
        for id in gattLinks.keys { sent = gatt.send(bytes, to: id) || sent }
        return sent
    }

    // MARK: Matching a phone's HELLO to a configured peer

    /// By remembered installation ID, else by a unique Bluetooth name (CoreBluetooth exposes no addresses).
    private func match(_ peer: PeerGattCentral.Peer) -> String? {
        if let address = Settings.gattPeerAddresses[peer.installationId],
           addresses.contains(where: { sameAddress($0, address) }) {
            return address
        }
        let name = peer.name.trimmingCharacters(in: .whitespaces)
        let candidates = addresses.filter {
            Self.name(of: $0).trimmingCharacters(in: .whitespaces).caseInsensitiveCompare(name) == .orderedSame
        }
        guard candidates.count == 1, let address = candidates.first else {
            if unmatched[peer.installationId] == nil {
                Log.write("Peers: Bluetooth LE peer '\(peer.name)' matches \(candidates.count) configured peers; keeping it idle for setup")
            }
            return nil
        }
        var known = Settings.gattPeerAddresses
        known[peer.installationId] = address
        Settings.gattPeerAddresses = known
        Log.write("Peers: Bluetooth LE peer '\(peer.name)' is \(address); remembered")
        return address
    }

    private func gattReady(_ peer: PeerGattCentral.Peer) {
        guard let address = match(peer) else {
            unmatched[peer.installationId] = peer
            return
        }
        unmatched[peer.installationId] = nil
        let wasReachable = isReachable(address)
        gattLinks[peer.installationId] = address
        if !wasReachable { onConnectionChange?(address, true) }
    }

    private func gattEnded(_ peer: PeerGattCentral.Peer) {
        unmatched[peer.installationId] = nil
        guard let address = gattLinks.removeValue(forKey: peer.installationId) else { return }
        if !isReachable(address) { onConnectionChange?(address, false) }
    }
}

func sameAddress(_ a: String, _ b: String) -> Bool { normalizedAddress(a) == normalizedAddress(b) }
