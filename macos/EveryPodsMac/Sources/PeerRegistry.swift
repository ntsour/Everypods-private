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

    var onPacket: ((Data) -> Void)?
    /// `connected` is the state of the peer that changed.
    var onConnectionChange: ((String, Bool) -> Void)?

    /// Configured peers by address, in the order they were added.
    private var addresses: [String] = []
    private var started = false
    private let gatt = PeerGattCentral()
    /// Ready links: installation ID → configured peer address.
    private var gattLinks: [String: String] = [:]

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
        linked.forEach { onConnectionChange?($0, false) }
    }

    /// Rebuilds the links (after wake, or when the Bluetooth stack was reset).
    func restart() {
        guard started else { return }
        stop()
        start()
    }

    /// A phone just authorized this Mac: retry the phones that refused it, without dropping the other links.
    func retryRefused() { gatt.retryRefused() }

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
            Log.write("Peers: Bluetooth LE peer '\(peer.name)' matches \(candidates.count) configured peers; not using it")
            return nil
        }
        var known = Settings.gattPeerAddresses
        known[peer.installationId] = address
        Settings.gattPeerAddresses = known
        Log.write("Peers: Bluetooth LE peer '\(peer.name)' is \(address); remembered")
        return address
    }

    private func gattReady(_ peer: PeerGattCentral.Peer) {
        guard let address = match(peer) else { return }
        let wasReachable = isReachable(address)
        gattLinks[peer.installationId] = address
        if !wasReachable { onConnectionChange?(address, true) }
    }

    private func gattEnded(_ peer: PeerGattCentral.Peer) {
        guard let address = gattLinks.removeValue(forKey: peer.installationId) else { return }
        if !isReachable(address) { onConnectionChange?(address, false) }
    }
}

func sameAddress(_ a: String, _ b: String) -> Bool { normalizedAddress(a) == normalizedAddress(b) }
