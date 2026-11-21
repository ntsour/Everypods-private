// PeerLink.swift
//
// One link to one peer (phone or computer) on the shared "EveryPodsCrossDevice" service. The link is either
// dialled by us (with backoff) or adopted from a channel the peer opened to our listener (PeerServer).
// Delivers 4-byte packets (and data frames) to `onPacket`.

import Foundation
import IOBluetooth

final class PeerLink: NSObject, IOBluetoothRFCOMMChannelDelegate {
    var onPacket: ((Data) -> Void)?
    var onConnectionChange: ((Bool) -> Void)?
    private(set) var isConnected = false
    /// True when the current channel was opened by us (as opposed to by the peer).
    private(set) var isOutbound = false

    let address: String
    private var device: IOBluetoothDevice?
    private var channel: IOBluetoothRFCOMMChannel?
    private var lookup: ServiceLookup?
    private var generation = 0
    private let initialDelay: TimeInterval = 1.5
    private let maxDelay: TimeInterval = 15
    private lazy var retryDelay: TimeInterval = initialDelay
    private var running = false

    init(address: String) {
        self.address = address
        super.init()
    }

    var name: String { IOBluetoothDevice(addressString: normalizedAddress(address))?.name ?? address }

    func start() {
        stop()
        guard let device = IOBluetoothDevice(addressString: normalizedAddress(address)) else {
            Log.write("PeerLink[\(name)]: no device for \(address)")
            return
        }
        self.device = device
        running = true
        generation += 1
        connect(generation)
    }

    func stop() {
        running = false
        generation += 1
        _ = channel?.close()
        channel = nil
        setConnected(false)
    }

    @discardableResult
    func send(_ bytes: [UInt8]) -> Bool {
        guard isConnected, let channel else { return false }
        var buffer = bytes
        let status = channel.writeSync(&buffer, length: UInt16(buffer.count))
        if status != kIOReturnSuccess { Log.write("PeerLink[\(name)]: write failed \(status)") }
        return status == kIOReturnSuccess
    }

    /// Takes over a channel the peer opened to our listener, unless a link to this peer is already up.
    func adopt(_ incoming: IOBluetoothRFCOMMChannel) {
        if isConnected {
            // Legacy links carry no installation ID yet, so the link that came up first is kept.
            Log.write("PeerLink[\(name)]: a link is already up; closing the incoming one")
            _ = incoming.close()
            return
        }
        let old = channel
        channel = incoming
        isOutbound = false
        incoming.setDelegate(self)
        generation += 1  // cancels any dial or retry in progress
        retryDelay = initialDelay
        if let old, old !== incoming { _ = old.close() }
        Log.write("PeerLink[\(name)]: connected (inbound)")
        if running { setConnected(true) } else {
            // A peer that dials in is accepted even before start() ran (the peer was just added).
            running = true
            setConnected(true)
        }
    }

    private func connect(_ token: Int) {
        guard running, token == generation, let device else { return }
        // A link the peer opened counts as the link to that peer: do not dial while it is up.
        if isConnected { return }
        let lookup = ServiceLookup(device: device, uuidBytes: ServiceUUID.crossDevice)
        self.lookup = lookup
        lookup.find { [weak self] channelID in
            guard let self, self.running, token == self.generation, !self.isConnected else { return }
            guard let channelID else { return self.scheduleRetry(token, "service not found") }
            var opened: IOBluetoothRFCOMMChannel?
            let status = device.openRFCOMMChannelAsync(&opened, withChannelID: channelID, delegate: self)
            Log.write("PeerLink[\(name)]: opening channel \(channelID): \(status == kIOReturnSuccess ? "ok" : "error \(status)")")
            if status != kIOReturnSuccess { return self.scheduleRetry(token, "open failed") }
            self.channel = opened
            self.isOutbound = true
            // If neither OpenComplete nor Closed arrives, retry.
            DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
                guard let self, token == self.generation, !self.isConnected else { return }
                self.scheduleRetry(token, "open timed out")
            }
        }
    }

    private func scheduleRetry(_ token: Int, _ reason: String) {
        guard running, token == generation else { return }
        _ = channel?.close()
        channel = nil
        // A timed-out open usually means the Mac's Bluetooth session with the phone is wedged (for example
        // after the app was killed with a channel open). Dropping the baseband link makes the next try start clean.
        if reason.contains("timed out"), let device {
            let status = device.closeConnection()
            Log.write("PeerLink[\(name)]: dropped baseband link to reset (\(status == kIOReturnSuccess ? "ok" : "error \(status)"))")
        }
        Log.write("PeerLink[\(name)]: retry in \(Int(retryDelay))s (\(reason))")
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, maxDelay)
        generation += 1
        let next = generation
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.connect(next) }
    }

    private func setConnected(_ value: Bool) {
        guard isConnected != value else { return }
        isConnected = value
        onConnectionChange?(value)
    }

    // MARK: IOBluetoothRFCOMMChannelDelegate

    func rfcommChannelOpenComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, status error: IOReturn) {
        guard rfcommChannel === channel else { return }
        guard error == kIOReturnSuccess else {
            Log.write("PeerLink[\(name)]: open failed \(String(format: "0x%08X", UInt32(bitPattern: error)))")
            return scheduleRetry(generation, "open failed")
        }
        retryDelay = initialDelay
        if !isConnected {
            Log.write("PeerLink[\(name)]: connected")
            setConnected(true)
        }
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!, length dataLength: Int) {
        guard rfcommChannel === channel else { return }
        onPacket?(Data(bytes: dataPointer, count: dataLength))
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        // A channel we closed on purpose (the losing link of a race, or a replaced one) reports here too.
        guard rfcommChannel === channel else { return }
        Log.write("PeerLink[\(name)]: closed")
        setConnected(false)
        channel = nil
        scheduleRetry(generation, "closed")
    }
}
