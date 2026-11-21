// ServiceLookup.swift
//
// Finds the RFCOMM channel for a service UUID on a paired phone. IOBluetooth's SDP query callback
// is unreliable, so a query is started for freshness and the cached records are used after a timeout.

import Foundation
import IOBluetooth

final class ServiceLookup: NSObject {
    private let device: IOBluetoothDevice
    private let uuid: IOBluetoothSDPUUID
    private var completion: ((BluetoothRFCOMMChannelID?) -> Void)?

    init(device: IOBluetoothDevice, uuidBytes: [UInt8]) {
        self.device = device
        self.uuid = IOBluetoothSDPUUID(data: Data(uuidBytes))
    }

    /// Calls `completion` once, on the main queue.
    func find(completion: @escaping (BluetoothRFCOMMChannelID?) -> Void) {
        self.completion = completion
        let status = device.performSDPQuery(self, uuids: [uuid])
        if status != kIOReturnSuccess {
            Log.write("SDP query failed to start: \(String(format: "0x%08X", UInt32(bitPattern: status)))")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in self?.finish() }
    }

    @objc func sdpQueryComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        finish()
    }

    private func finish() {
        guard let completion else { return }
        self.completion = nil
        let records = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
        for record in records where record.hasService(from: [uuid]) {
            var channel: BluetoothRFCOMMChannelID = 0
            if record.getRFCOMMChannelID(&channel) == kIOReturnSuccess {
                completion(channel)
                return
            }
        }
        completion(nil)
    }
}
