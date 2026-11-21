// AirPodsConnector.swift
//
// Connects and disconnects the paired AirPods at the Bluetooth level. The AirPods must be paired
// with this Mac in System Settings first (a stale pairing fails with KEY_NOT_FOUND).

import Foundation
import IOBluetooth

final class AirPodsConnector: NSObject {
    private var device: IOBluetoothDevice? {
        Settings.airPodsAddress.flatMap { IOBluetoothDevice(addressString: normalizedAddress($0)) }
    }
    private var pending: ((Bool) -> Void)?

    var isConfigured: Bool { device != nil }
    var isConnected: Bool { device?.isConnected() ?? false }
    var name: String { device?.name ?? "AirPods" }

    /// Connects; calls `completion(true)` on success. Times out after 20 seconds.
    func connect(completion: @escaping (Bool) -> Void) {
        guard let device else { return completion(false) }
        if device.isConnected() { return completion(true) }
        pending = completion
        let status = device.openConnection(self)
        Log.write("AirPods: openConnection \(status == kIOReturnSuccess ? "started" : "error \(status)")")
        if status != kIOReturnSuccess { return finish(false) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in self?.finish(false) }
    }

    /// Connects with up to three attempts (immediately, after 1.5 s, after 3 s), as the Windows client does:
    /// the phone may not have released the AirPods yet on the first try.
    func connectWithRetries(completion: @escaping (Bool) -> Void) {
        let waits: [TimeInterval] = [0, 1.5, 3]
        func attempt(_ index: Int) {
            Log.write("AirPods: connect attempt \(index + 1)/\(waits.count)")
            connect { success in
                if success { return completion(true) }
                guard index + 1 < waits.count else { return completion(false) }
                DispatchQueue.main.asyncAfter(deadline: .now() + waits[index + 1]) { attempt(index + 1) }
            }
        }
        attempt(0)
    }

    @discardableResult
    func disconnect() -> Bool {
        guard let device else { return false }
        let status = device.closeConnection()
        Log.write("AirPods: closeConnection \(status == kIOReturnSuccess ? "ok" : "error \(status)")")
        return status == kIOReturnSuccess
    }

    @objc func connectionComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        Log.write("AirPods: connectionComplete status=\(String(format: "0x%08X", UInt32(bitPattern: status)))")
        // isConnected can lag the callback by a moment, so a success status is trusted.
        finish(status == kIOReturnSuccess)
    }

    private func finish(_ success: Bool) {
        guard let completion = pending else { return }
        pending = nil
        completion(success)
    }
}
