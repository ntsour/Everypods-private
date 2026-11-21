// PeerRegistry.swift
//
// The peers (phones and other computers) this Mac coordinates with: one RFCOMM link per peer, dialled by us or
// accepted from the listener; packets are broadcast to every connected peer. Mirrors windows/src/PeerRegistry.

import Foundation
import IOBluetooth

final class PeerRegistry {
    struct Info {
        let address: String
        let name: String
        let connected: Bool
    }

    var onPacket: ((Data) -> Void)?
    /// `connected` is the state of the phone that changed.
    var onConnectionChange: ((String, Bool) -> Void)?

    private var links: [PeerLink] = []
    private var started = false
    private let server = PeerServer()

    var isAnyConnected: Bool { links.contains { $0.isConnected } }
    var isEmpty: Bool { links.isEmpty }

    var infos: [Info] {
        links.map { Info(address: $0.address, name: $0.name, connected: $0.isConnected) }
    }

    /// Starts the links for the saved peers.
    func start() {
        started = true
        for address in Settings.peers where !links.contains(where: { sameAddress($0.address, address) }) {
            add(address, persist: false)
        }
        server.onChannel = { [weak self] channel, device in self?.accept(channel, from: device) }
        server.start()
        links.forEach { $0.start() }
    }

    func stop() {
        started = false
        server.stop()
        links.forEach { $0.stop() }
    }

    /// A peer opened a link to us. An unknown device is saved as a peer.
    private func accept(_ channel: IOBluetoothRFCOMMChannel, from device: IOBluetoothDevice?) {
        guard let address = device?.addressString else {
            Log.write("Peers: incoming channel without an address; closing")
            _ = channel.close()
            return
        }
        if let known = links.first(where: { sameAddress($0.address, address) }) {
            return known.adopt(channel)
        }
        Log.write("Peers: unknown device \(device?.name ?? address) dialled in; saving it as a peer")
        add(address)
        links.first(where: { sameAddress($0.address, address) })?.adopt(channel)
    }

    /// Rebuilds every link from scratch (after wake, or when the Bluetooth stack was reset).
    func restart() {
        links.forEach { $0.stop() }
        links.forEach { $0.start() }
    }

    func add(_ address: String, persist: Bool = true) {
        guard !links.contains(where: { sameAddress($0.address, address) }) else { return }
        let link = PeerLink(address: address)
        link.onPacket = { [weak self] data in self?.onPacket?(data) }
        link.onConnectionChange = { [weak self] connected in self?.onConnectionChange?(address, connected) }
        links.append(link)
        if persist {
            Settings.peers = Settings.peers + [address]
        }
        if started { link.start() }
        Log.write("Peers: added \(link.name)")
    }

    func remove(_ address: String) {
        guard let index = links.firstIndex(where: { sameAddress($0.address, address) }) else { return }
        let link = links.remove(at: index)
        link.stop()
        Settings.peers = Settings.peers.filter { !sameAddress($0, address) }
        Log.write("Peers: removed \(link.name)")
        onConnectionChange?(address, false)
    }

    /// Sends to every connected peer; returns true if at least one accepted the packet.
    @discardableResult
    func send(_ bytes: [UInt8]) -> Bool {
        var sent = false
        for link in links where link.isConnected { sent = link.send(bytes) || sent }
        return sent
    }
}

func sameAddress(_ a: String, _ b: String) -> Bool { normalizedAddress(a) == normalizedAddress(b) }
