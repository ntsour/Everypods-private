// SetupAuthorizer.swift
//
// One-off authorization: the Android app only accepts peers that completed its BT handover setup
// exchange (BtHandoverSync.kt). Before running this, add this Mac as a peer in the Android app.
//
// Wire format per exchange: 4-byte big-endian length, then UTF-8 JSON
//   {"version":1,"revision":N,"origin":"<uuid>","enabled":true}
// Two exchanges are needed: the first (revision 1) loses the merge and returns the phone's state;
// the second echoes that state back, which clears the phone's "pending" marker for this Mac.

import Foundation
import IOBluetooth

final class SetupAuthorizer: NSObject, IOBluetoothRFCOMMChannelDelegate {
    private let device: IOBluetoothDevice
    private var lookup: ServiceLookup?
    private var channel: IOBluetoothRFCOMMChannel?
    private var payload = Data()
    private var received = Data()
    private var onReply: ((Result<String, Error>) -> Void)?

    enum Failure: Error, LocalizedError {
        case serviceNotFound, openFailed, closed, timeout
        var errorDescription: String? {
            switch self {
            case .serviceNotFound: return "The phone's setup service was not found. Is the Mac added in the Android app?"
            case .openFailed: return "Could not open the setup channel."
            case .closed: return "The phone closed the setup channel."
            case .timeout: return "The phone did not answer in time."
            }
        }
    }

    init?(phoneAddress: String) {
        guard let device = IOBluetoothDevice(addressString: normalizedAddress(phoneAddress)) else { return nil }
        self.device = device
    }

    func run(completion: @escaping (Result<Void, Error>) -> Void) {
        let origin = UUID().uuidString.lowercased()
        let first = "{\"version\":1,\"revision\":1,\"origin\":\"\(origin)\",\"enabled\":true}"
        exchange(first) { [weak self] result in
            switch result {
            case .failure(let error): completion(.failure(error))
            case .success(let phoneState):
                Log.write("Authorize: phone state \(phoneState); echoing it back")
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    self?.exchange(phoneState) { second in
                        completion(second.map { _ in () })
                    }
                }
            }
        }
    }

    private func exchange(_ json: String, completion: @escaping (Result<String, Error>) -> Void) {
        let body = Data(json.utf8)
        var framed = Data()
        var length = UInt32(body.count).bigEndian
        withUnsafeBytes(of: &length) { framed.append(contentsOf: $0) }
        framed.append(body)
        payload = framed
        received = Data()
        onReply = completion
        let lookup = ServiceLookup(device: device, uuidBytes: ServiceUUID.handoverSetup)
        self.lookup = lookup
        lookup.find { [weak self] channelID in
            guard let self else { return }
            Log.write("Authorize: setup service channel \(channelID.map(String.init) ?? "not found")")
            guard let channelID else { return self.finish(.failure(Failure.serviceNotFound)) }
            var opened: IOBluetoothRFCOMMChannel?
            let status = self.device.openRFCOMMChannelAsync(&opened, withChannelID: channelID, delegate: self)
            if status != kIOReturnSuccess { return self.finish(.failure(Failure.openFailed)) }
            self.channel = opened
            DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in self?.finish(.failure(Failure.timeout)) }
        }
    }

    private func finish(_ result: Result<String, Error>) {
        guard let handler = onReply else { return }
        onReply = nil
        switch result {
        case .success(let text): Log.write("Authorize: reply \(text)")
        case .failure(let error): Log.write("Authorize: failed (\(error.localizedDescription))")
        }
        _ = channel?.close()
        channel = nil
        handler(result)
    }

    func rfcommChannelOpenComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, status error: IOReturn) {
        Log.write("Authorize: channel open status \(String(format: "0x%08X", UInt32(bitPattern: error)))")
        guard error == kIOReturnSuccess else { return finish(.failure(Failure.openFailed)) }
        var bytes = [UInt8](payload)
        _ = rfcommChannel.writeSync(&bytes, length: UInt16(bytes.count))
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!, length dataLength: Int) {
        received.append(Data(bytes: dataPointer, count: dataLength))
        guard received.count >= 4 else { return }
        let length = received.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        guard received.count >= 4 + length else { return }
        let text = String(data: received.subdata(in: 4..<(4 + length)), encoding: .utf8) ?? ""
        finish(.success(text))
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        finish(.failure(Failure.closed))
    }
}
