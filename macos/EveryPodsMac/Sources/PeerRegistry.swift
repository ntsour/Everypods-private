// PeerRegistry.swift
//
// The set of Android phones this Mac coordinates with: one persistent RFCOMM link per phone, packets are
// broadcast to every connected phone. Mirrors windows/src/PeerRegistry.

import Foundation

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

    var isAnyConnected: Bool { links.contains { $0.isConnected } }
    var isEmpty: Bool { links.isEmpty }

    var infos: [Info] {
        links.map { Info(address: $0.address, name: $0.name, connected: $0.isConnected) }
    }

    /// Starts the links for the saved phones.
    func start() {
        started = true
        for address in Settings.phones where !links.contains(where: { sameAddress($0.address, address) }) {
            add(address, persist: false)
        }
        links.forEach { $0.start() }
    }

    func stop() {
        started = false
        links.forEach { $0.stop() }
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
            var phones = Settings.phones
            if !phones.contains(where: { sameAddress($0, address) }) { phones.append(address) }
            Settings.phones = phones
        }
        if started { link.start() }
        Log.write("Peers: added \(link.name)")
    }

    func remove(_ address: String) {
        guard let index = links.firstIndex(where: { sameAddress($0.address, address) }) else { return }
        let link = links.remove(at: index)
        link.stop()
        Settings.phones = Settings.phones.filter { !sameAddress($0, address) }
        Log.write("Peers: removed \(link.name)")
        onConnectionChange?(address, false)
    }

    /// Sends to every connected phone; returns true if at least one accepted the packet.
    @discardableResult
    func send(_ bytes: [UInt8]) -> Bool {
        var sent = false
        for link in links where link.isConnected { sent = link.send(bytes) || sent }
        return sent
    }
}

func sameAddress(_ a: String, _ b: String) -> Bool { normalizedAddress(a) == normalizedAddress(b) }
