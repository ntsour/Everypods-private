// ServiceLookup.swift
//
// Finds the RFCOMM channel for a service UUID on a paired device, from the records macOS has cached.
//
// An SDP query is sent only when nothing is cached, and at most once per device every two minutes. Each query
// to an absent device costs bluetoothd three page attempts (~15 s), and it queues every outgoing connection
// behind them: issuing one per dial built a backlog that kept the AirPods connect waiting for 40 s and more.
// IOBluetooth's query callback is unreliable anyway, so the cache is read after a short wait.

import Foundation
import IOBluetooth

final class ServiceLookup: NSObject {
    private let device: IOBluetoothDevice
    private let uuid: IOBluetoothSDPUUID
    private var completion: ((BluetoothRFCOMMChannelID?) -> Void)?
    private static var lastQuery: [String: Date] = [:]
    private static let queryInterval: TimeInterval = 120

    init(device: IOBluetoothDevice, uuidBytes: [UInt8]) {
        self.device = device
        self.uuid = IOBluetoothSDPUUID(data: Data(uuidBytes))
    }

    /// Calls `completion` once, on the main queue. `fresh` always sends a query first (a user-started setup, where
    /// the phone's channel may have moved since it was cached).
    func find(fresh: Bool = false, completion: @escaping (BluetoothRFCOMMChannelID?) -> Void) {
        self.completion = completion
        if !fresh, cachedChannel() != nil {
            DispatchQueue.main.async { [weak self] in self?.finish() }
            return
        }
        let key = device.addressString ?? ""
        if !fresh, let last = Self.lastQuery[key], Date().timeIntervalSince(last) < Self.queryInterval {
            DispatchQueue.main.async { [weak self] in self?.finish() }
            return
        }
        Self.lastQuery[key] = Date()
        let status = device.performSDPQuery(self, uuids: [uuid])
        if status != kIOReturnSuccess {
            Log.write("SDP query failed to start: \(String(format: "0x%08X", UInt32(bitPattern: status)))")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in self?.finish() }
    }

    private func cachedChannel() -> BluetoothRFCOMMChannelID? {
        let records = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
        for record in records where record.hasService(from: [uuid]) {
            var channel: BluetoothRFCOMMChannelID = 0
            if record.getRFCOMMChannelID(&channel) == kIOReturnSuccess { return channel }
        }
        return nil
    }

    @objc func sdpQueryComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        finish()
    }

    private func finish() {
        guard let completion else { return }
        self.completion = nil
        completion(cachedChannel())
    }
}
