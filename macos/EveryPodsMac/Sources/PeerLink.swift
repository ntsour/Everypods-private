// PeerLink.swift
//
// Keeps one RFCOMM channel open to the Android phone's "EveryPodsCrossDevice" service and
// reconnects with backoff. Delivers 4-byte packets (and data frames) to `onPacket`.

import Foundation
import IOBluetooth

final class PeerLink: NSObject, IOBluetoothRFCOMMChannelDelegate {
    var onPacket: ((Data) -> Void)?
    var onConnectionChange: ((Bool) -> Void)?
    private(set) var isConnected = false

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

    private func connect(_ token: Int) {
        guard running, token == generation, let device else { return }
        let lookup = ServiceLookup(device: device, uuidBytes: ServiceUUID.crossDevice)
        self.lookup = lookup
        lookup.find { [weak self] channelID in
            guard let self, self.running, token == self.generation else { return }
            guard let channelID else { return self.scheduleRetry(token, "service not found") }
            var opened: IOBluetoothRFCOMMChannel?
            let status = device.openRFCOMMChannelAsync(&opened, withChannelID: channelID, delegate: self)
            Log.write("PeerLink[\(name)]: opening channel \(channelID): \(status == kIOReturnSuccess ? "ok" : "error \(status)")")
            if status != kIOReturnSuccess { return self.scheduleRetry(token, "open failed") }
            self.channel = opened
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
        guard error == kIOReturnSuccess else {
            Log.write("PeerLink[\(name)]: open failed \(String(format: "0x%08X", UInt32(bitPattern: error)))")
            return scheduleRetry(generation, "open failed")
        }
        channel = rfcommChannel
        retryDelay = initialDelay
        Log.write("PeerLink[\(name)]: connected")
        setConnected(true)
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!, length dataLength: Int) {
        onPacket?(Data(bytes: dataPointer, count: dataLength))
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        Log.write("PeerLink[\(name)]: closed")
        setConnected(false)
        channel = nil
        scheduleRetry(generation, "closed")
    }
}
