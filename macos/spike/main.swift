// main.swift
//
// Throwaway macOS spike. Proves two things with IOBluetooth:
//   1. rfcomm: open an RFCOMM channel to an Android phone's custom service and
//      exchange 4-byte packets.
//   2. airpods-connect / airpods-disconnect: connect or disconnect a paired device.
//   3. paired: list paired devices (name, MAC, connected state). Read-only.
//
// Build:
//   See README.md. The Info.plist (NSBluetoothAlwaysUsageDescription) must be embedded
//   or macOS aborts the process on first Bluetooth access:
//   swiftc -O -framework IOBluetooth -framework Foundation \
//     -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist \
//     main.swift -o spike

import AppKit
import Foundation
import IOBluetooth

// MARK: - Logging

private let timestampFormatter: DateFormatter = {
    let formatter = DateFormatter()
    formatter.dateFormat = "HH:mm:ss.SSS"
    return formatter
}()

/// Prints a timestamped line and flushes stdout so output appears immediately, even when piped.
func log(_ message: String) {
    print("[\(timestampFormatter.string(from: Date()))] \(message)")
    fflush(stdout)
}

/// Renders an IOReturn as hex, naming the few codes this spike is likely to see.
func describe(_ status: IOReturn) -> String {
    let bits = UInt32(bitPattern: status)
    let hex = String(format: "0x%08X", bits)
    switch bits {
    case 0x0000_0000: return "\(hex) (kIOReturnSuccess)"
    case 0xE000_02BC: return "\(hex) (kIOReturnError)"
    case 0xE000_02D6: return "\(hex) (kIOReturnTimeout)"
    default: return hex
    }
}

/// Formats bytes as "00 01 00 01".
func hexString(_ data: Data) -> String {
    data.map { String(format: "%02X", $0) }.joined(separator: " ")
}

// MARK: - Helpers

/// Accepts AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF and returns the uppercase dash form.
func normalizedAddress(_ raw: String) -> String? {
    let dashed = raw.uppercased().replacingOccurrences(of: ":", with: "-")
    let parts = dashed.split(separator: "-", omittingEmptySubsequences: false)
    guard parts.count == 6,
          parts.allSatisfy({ $0.count == 2 && $0.allSatisfy { $0.isHexDigit } }) else {
        return nil
    }
    return dashed
}

/// Resolves a MAC argument to an IOBluetoothDevice or exits with an error.
func deviceFromArgument(_ raw: String) -> IOBluetoothDevice {
    guard let address = normalizedAddress(raw) else {
        log("Invalid MAC '\(raw)'. Expected AA:BB:CC:DD:EE:FF")
        exit(2)
    }
    guard let device = IOBluetoothDevice(addressString: address) else {
        log("IOBluetoothDevice(addressString:) returned nil for \(address)")
        exit(2)
    }
    return device
}

/// Reads stdin on a background thread and hands each trimmed line to the main queue.
/// The main queue is serviced by RunLoop.main, so handlers run alongside IOBluetooth callbacks.
func startStdinReader(_ handler: @escaping (String) -> Void) {
    DispatchQueue.global(qos: .userInitiated).async {
        while let line = readLine() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            DispatchQueue.main.async { handler(trimmed) }
        }
    }
}

/// Runs the main loop. SPIKE_NSAPP=1 uses a real NSApplication loop (IOBluetooth callbacks
/// may need one), otherwise a plain RunLoop.
func runMainLoop() -> Never {
    if ProcessInfo.processInfo.environment["SPIKE_NSAPP"] != nil {
        log("Using NSApplication run loop")
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        app.run()
    } else {
        RunLoop.main.run()
    }
    exit(0)
}

// MARK: - Mode: paired

/// Lists paired devices. Does not connect to anything.
func listPaired() -> Never {
    let devices = (IOBluetoothDevice.pairedDevices() as? [IOBluetoothDevice]) ?? []
    log("Paired devices: \(devices.count)")
    for device in devices {
        let name = device.name ?? "(no name)"
        let address = device.addressString ?? "(no address)"
        log("  \(name) | \(address) | isConnected=\(device.isConnected())")
    }
    exit(0)
}

// MARK: - Mode: rfcomm

/// UUID of the custom service the Android app advertises: 1abbb9a4-10e4-4000-a75c-8953c5471342
let serviceUUIDBytes: [UInt8] = [
    0x1a, 0xbb, 0xb9, 0xa4, 0x10, 0xe4, 0x40, 0x00,
    0xa7, 0x5c, 0x89, 0x53, 0xc5, 0x47, 0x13, 0x42
]

/// Keeps the SDP query, the RFCOMM channel and its delegate alive for the whole run.
/// NSObject subclass is required: IOBluetooth invokes delegate methods via the Objective-C runtime.
final class RFCOMMSpike: NSObject, IOBluetoothRFCOMMChannelDelegate {
    private let device: IOBluetoothDevice
    private var channel: IOBluetoothRFCOMMChannel?
    var onOpen: (() -> Void)?
    var onData: ((Data) -> Void)?

    init(device: IOBluetoothDevice) {
        self.device = device
    }

    /// Step 1: SDP query to discover the RFCOMM channel for our service UUID.
    func start() {
        let cached = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
        log("Cached SDP records: \(cached.count); isConnected=\(device.isConnected()) isPaired=\(device.isPaired())")
        for record in cached {
            var cid: BluetoothRFCOMMChannelID = 0
            let rc = record.getRFCOMMChannelID(&cid)
            let isTarget = record.hasService(from: [IOBluetoothSDPUUID(data: Data(serviceUUIDBytes))])
            log("  cached: \(record.getServiceName() ?? "(unnamed)") rfcomm=\(rc == kIOReturnSuccess ? String(cid) : "-") target=\(isTarget)")
        }
        log("SDP query on \(device.addressString ?? "?")")
        let status = device.performSDPQuery(self, uuids: [IOBluetoothSDPUUID(data: Data(serviceUUIDBytes))])
        log("performSDPQuery returned \(describe(status))")
        if status != kIOReturnSuccess {
            exit(1)
        }
        // If IOBluetooth never calls back, fall through to whatever is cached after 8 s.
        DispatchQueue.main.asyncAfter(deadline: .now() + 8) { [weak self] in
            guard let self, self.channel == nil else { return }
            log("No SDP callback after 8s; trying cached records")
            self.sdpQueryComplete(self.device, status: kIOReturnSuccess)
        }
    }

    /// Informal delegate selector called by IOBluetooth when performSDPQuery completes.
    @objc func sdpQueryComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        log("SDP query complete: \(describe(status))")
        guard status == kIOReturnSuccess else { exit(1) }

        let targetUUID = IOBluetoothSDPUUID(data: Data(serviceUUIDBytes))
        let records = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
        log("SDP records on device: \(records.count)")

        guard let record = records.first(where: { $0.hasService(from: [targetUUID]) }) else {
            log("Service 1abbb9a4-10e4-4000-a75c-8953c5471342 not found in SDP records")
            exit(1)
        }

        var channelID: BluetoothRFCOMMChannelID = 0
        let channelStatus = record.getRFCOMMChannelID(&channelID)
        if channelStatus != kIOReturnSuccess {
            log("getRFCOMMChannelID failed: \(describe(channelStatus))")
            exit(1)
        }
        if let override = ProcessInfo.processInfo.environment["SPIKE_CHANNEL"], let value = UInt8(override) {
            channelID = value
            log("Overriding channel with SPIKE_CHANNEL=\(value)")
        }
        log("Found service on RFCOMM channel \(channelID); opening")

        // Step 2: asynchronous RFCOMM open. Results arrive via the delegate methods below.
        var newChannel: IOBluetoothRFCOMMChannel?
        let useSync = ProcessInfo.processInfo.environment["SPIKE_SYNC"] != nil
        let openStatus = useSync
            ? device.openRFCOMMChannelSync(&newChannel, withChannelID: channelID, delegate: self)
            : device.openRFCOMMChannelAsync(&newChannel, withChannelID: channelID, delegate: self)
        log("open (\(useSync ? "sync" : "async")) returned \(describe(openStatus)); channel=\(newChannel == nil ? "nil" : "set")")
        if useSync && openStatus == kIOReturnSuccess { log("OPEN (sync)") }
        if openStatus != kIOReturnSuccess {
            log("openRFCOMMChannelAsync failed immediately: \(describe(openStatus))")
            exit(1)
        }
        channel = newChannel
    }

    // MARK: IOBluetoothRFCOMMChannelDelegate

    func rfcommChannelOpenComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, status error: IOReturn) {
        guard error == kIOReturnSuccess else {
            log("RFCOMM open failed: \(describe(error))")
            exit(1)
        }
        channel = rfcommChannel
        log("OPEN")
        onOpen?()
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!,
                           length dataLength: Int) {
        let bytes = Data(bytes: dataPointer, count: dataLength)
        log("RX (\(dataLength) bytes): \(hexString(bytes))")
        onData?(bytes)
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        log("CLOSED")
        exit(0)
    }

    // MARK: Sending

    /// Writes a payload synchronously. Logs the IOReturn from writeSync.
    func send(_ payload: [UInt8], label: String) {
        guard let channel else {
            log("Channel not open yet; cannot send \(label)")
            return
        }
        var buffer = payload
        let status = channel.writeSync(&buffer, length: UInt16(buffer.count))
        log("TX \(label) (\(payload.count) bytes): \(hexString(Data(payload))) status=\(describe(status))")
    }

    /// Closes the channel and exits after a short grace period for the close callback.
    func shutdown() {
        if let channel {
            _ = channel.close()
            log("RFCOMM channel close requested")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(0) }
    }
}

/// Global reference so the spike (and its delegate chain) outlives runRFCOMM.
var activeRFCOMMSpike: RFCOMMSpike?

func runRFCOMM(address: String) -> Never {
    let device = deviceFromArgument(address)
    let spike = RFCOMMSpike(device: device)
    activeRFCOMMSpike = spike

    startStdinReader { line in
        switch line.lowercased() {
        case "connected":    spike.send([0x00, 0x01, 0x00, 0x01], label: "connected")
        case "disconnected": spike.send([0x00, 0x01, 0x00, 0x00], label: "disconnected")
        case "reqdisc":      spike.send([0x00, 0x02, 0x00, 0x00], label: "reqdisc")
        case "reqhand":      spike.send([0x00, 0x02, 0x00, 0x04], label: "reqhand")
        case "quit":
            log("quit requested")
            spike.shutdown()
        case "":
            break
        default:
            log("Unknown command '\(line)'. Use: connected | disconnected | reqdisc | reqhand | quit")
        }
    }

    // Auto-close after 60 seconds.
    DispatchQueue.main.asyncAfter(deadline: .now() + 60) {
        log("60s elapsed; closing")
        spike.shutdown()
    }

    spike.start()
    runMainLoop()
}

// MARK: - Mode: setup-server

/// Hosts the "EveryPodsBTHandoverSetup" RFCOMM service (UUID de51a5b3-4140-4fb7-a524-967379181773)
/// that the Android app connects to when a peer is added. Wire format, from BtHandoverSync.kt:
///   4-byte big-endian length, then UTF-8 JSON {"version":1,"revision":N,"origin":"<uuid>","enabled":true}
/// The peer replies with its own state in the same format. This spike echoes the request back as the
/// acknowledgement, which is the minimum the Android merge needs to clear its "pending" marker.
let setupServiceUUIDBytes: [UInt8] = [
    0xde, 0x51, 0xa5, 0xb3, 0x41, 0x40, 0x4f, 0xb7,
    0xa5, 0x24, 0x96, 0x73, 0x79, 0x18, 0x17, 0x73
]

final class SetupServer: NSObject, IOBluetoothRFCOMMChannelDelegate {
    private var serviceRecord: IOBluetoothSDPServiceRecord?
    private var notification: IOBluetoothUserNotification?
    private var buffers: [ObjectIdentifier: Data] = [:]
    private var channels: [IOBluetoothRFCOMMChannel] = []

    func start() {
        let uuid = IOBluetoothSDPUUID(data: Data(setupServiceUUIDBytes))
        let l2cap = IOBluetoothSDPUUID(uuid16: 0x0100)!
        let rfcomm = IOBluetoothSDPUUID(uuid16: 0x0003)!
        let dict: [String: Any] = [
            "0001 - ServiceClassIDList": [uuid],
            "0004 - ProtocolDescriptorList": [
                [l2cap],
                [rfcomm, ["DataElementSize": 1, "DataElementType": 1, "DataElementValue": 1]]
            ],
            "0100 - ServiceName*": "EveryPodsBTHandoverSetup"
        ]
        guard let record = IOBluetoothSDPServiceRecord.publishedServiceRecord(with: dict) else {
            log("publishedServiceRecord(with:) failed")
            exit(1)
        }
        serviceRecord = record
        var channelID: BluetoothRFCOMMChannelID = 0
        let status = record.getRFCOMMChannelID(&channelID)
        log("Published service record; getRFCOMMChannelID=\(describe(status)) channel=\(channelID)")
        guard status == kIOReturnSuccess else { exit(1) }

        notification = IOBluetoothRFCOMMChannel.register(
            forChannelOpenNotifications: self,
            selector: #selector(channelOpened(_:channel:)),
            withChannelID: channelID,
            direction: kIOBluetoothUserNotificationChannelDirectionIncoming)
        log("Listening for incoming RFCOMM on channel \(channelID); notification=\(notification == nil ? "nil" : "ok")")
        log("Now add this Mac as a peer in the Android app (Cross-Device, BT handover). Ctrl-C to stop.")
    }

    @objc func channelOpened(_ note: IOBluetoothUserNotification!, channel: IOBluetoothRFCOMMChannel!) {
        let address = channel.getDevice()?.addressString ?? "?"
        log("INCOMING channel from \(address) mtu=\(channel.getMTU())")
        channels.append(channel)
        buffers[ObjectIdentifier(channel)] = Data()
        _ = channel.setDelegate(self)
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!, length dataLength: Int) {
        let key = ObjectIdentifier(rfcommChannel)
        var buffer = buffers[key] ?? Data()
        buffer.append(Data(bytes: dataPointer, count: dataLength))
        log("RX \(dataLength) bytes: \(hexString(Data(bytes: dataPointer, count: dataLength)))")
        guard buffer.count >= 4 else { buffers[key] = buffer; return }
        let length = buffer.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        guard length >= 1, length <= 1024 else {
            log("Invalid length \(length); closing")
            _ = rfcommChannel.close()
            return
        }
        guard buffer.count >= 4 + length else { buffers[key] = buffer; return }
        let body = buffer.subdata(in: 4..<(4 + length))
        buffers[key] = Data()
        guard let json = String(data: body, encoding: .utf8) else { log("Body is not UTF-8"); return }
        log("REQUEST JSON: \(json)")

        // Echo the request as the acknowledgement (same revision/origin, enabled stays as requested).
        var reply = Data()
        var big = UInt32(body.count).bigEndian
        withUnsafeBytes(of: &big) { reply.append(contentsOf: $0) }
        reply.append(body)
        var bytes = [UInt8](reply)
        let status = rfcommChannel.writeSync(&bytes, length: UInt16(bytes.count))
        log("TX ack (\(bytes.count) bytes) status=\(describe(status))")
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        log("CLOSED incoming channel")
        channels.removeAll { $0 === rfcommChannel }
    }
}

var activeSetupServer: SetupServer?

func runSetupServer() -> Never {
    let server = SetupServer()
    activeSetupServer = server
    server.start()
    runMainLoop()
}

// MARK: - Mode: setup-client

/// Acts as the client of the Pixel's own "EveryPodsBTHandoverSetup" listener (BtHandoverSync.kt).
/// The Pixel merges whatever state we send. Two exchanges are needed:
///   1. Send revision 1; the Pixel answers with its own (higher) state, which wins the merge.
///   2. Echo that exact state back; the merge now clears the Pixel's pending marker for this Mac.
final class SetupClient: NSObject, IOBluetoothRFCOMMChannelDelegate {
    private let device: IOBluetoothDevice
    private var channel: IOBluetoothRFCOMMChannel?
    private var receive = Data()
    private var payload = Data()
    private var onReply: ((String) -> Void)?

    init(device: IOBluetoothDevice) { self.device = device }

    /// Looks up the RFCOMM channel for the setup service in the cached SDP records.
    func channelID() -> BluetoothRFCOMMChannelID? {
        let uuid = IOBluetoothSDPUUID(data: Data(setupServiceUUIDBytes))
        let records = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
        guard let record = records.first(where: { $0.hasService(from: [uuid]) }) else { return nil }
        var id: BluetoothRFCOMMChannelID = 0
        return record.getRFCOMMChannelID(&id) == kIOReturnSuccess ? id : nil
    }

    /// One request/reply exchange on a fresh channel. `reply` receives the peer's JSON.
    func exchange(json: String, reply: @escaping (String) -> Void) {
        guard let id = channelID() else { log("Setup service not in SDP cache"); exit(1) }
        let body = Data(json.utf8)
        var framed = Data()
        var big = UInt32(body.count).bigEndian
        withUnsafeBytes(of: &big) { framed.append(contentsOf: $0) }
        framed.append(body)
        payload = framed
        receive = Data()
        onReply = reply
        log("Opening setup channel \(id); request JSON: \(json)")
        var opened: IOBluetoothRFCOMMChannel?
        let status = device.openRFCOMMChannelAsync(&opened, withChannelID: id, delegate: self)
        log("openRFCOMMChannelAsync returned \(describe(status))")
        if status != kIOReturnSuccess { exit(1) }
        channel = opened
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) {
            if self.onReply != nil { log("Timed out waiting for reply"); exit(1) }
        }
    }

    func rfcommChannelOpenComplete(_ rfcommChannel: IOBluetoothRFCOMMChannel!, status error: IOReturn) {
        log("setup channel open complete: \(describe(error))")
        guard error == kIOReturnSuccess else { exit(1) }
        channel = rfcommChannel
        var bytes = [UInt8](payload)
        let status = rfcommChannel.writeSync(&bytes, length: UInt16(bytes.count))
        log("TX request (\(bytes.count) bytes) status=\(describe(status))")
    }

    func rfcommChannelData(_ rfcommChannel: IOBluetoothRFCOMMChannel!,
                           data dataPointer: UnsafeMutableRawPointer!, length dataLength: Int) {
        receive.append(Data(bytes: dataPointer, count: dataLength))
        guard receive.count >= 4 else { return }
        let length = receive.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        guard receive.count >= 4 + length else { return }
        let text = String(data: receive.subdata(in: 4..<(4 + length)), encoding: .utf8) ?? "(not utf8)"
        log("RX reply: \(text)")
        let handler = onReply
        onReply = nil
        _ = rfcommChannel.close()
        handler?(text)
    }

    func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
        log("setup channel closed")
        if onReply != nil { log("Closed before a reply arrived"); exit(1) }
    }
}

var activeSetupClient: SetupClient?

func runSetupClient(address: String) -> Never {
    let device = deviceFromArgument(address)
    let client = SetupClient(device: device)
    activeSetupClient = client
    let origin = UUID().uuidString.lowercased()
    let first = "{\"version\":1,\"revision\":1,\"origin\":\"\(origin)\",\"enabled\":true}"
    client.exchange(json: first) { winner in
        log("Pixel state is \(winner); echoing it back to be accepted")
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            client.exchange(json: winner) { final in
                log("DONE. Final reply: \(final)")
                exit(0)
            }
        }
    }
    runMainLoop()
}

// MARK: - Mode: handover

/// Mac requests the AirPods from the phone: send kRequestDisconnect over the cross-device channel,
/// give the phone a moment to release, then connect the AirPods here. Logs timing at every step.
var activeHandoverSpike: RFCOMMSpike?
var activeHandoverConnect: ConnectDelegate?

func runHandover(phoneAddress: String, airpodsAddress: String) -> Never {
    let phone = deviceFromArgument(phoneAddress)
    let airpods = deviceFromArgument(airpodsAddress)
    log("Before: AirPods isConnected=\(airpods.isConnected())")
    let spike = RFCOMMSpike(device: phone)
    activeHandoverSpike = spike
    spike.onOpen = {
        let started = Date()
        spike.send([0x00, 0x02, 0x00, 0x00], label: "kRequestDisconnect")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
            log("Connecting AirPods on the Mac (phone had 2.5s to release)")
            let delegate = ConnectDelegate()
            delegate.completion = { status, connected in
                log("HANDOVER RESULT: status=\(describe(status)) isConnected=\(connected) after \(String(format: "%.1f", Date().timeIntervalSince(started)))s")
                DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                    log("3s later: AirPods isConnected=\(airpods.isConnected())")
                    spike.shutdown()
                }
            }
            activeHandoverConnect = delegate
            let rc = airpods.openConnection(delegate)
            log("openConnection returned \(describe(rc)); isConnected=\(airpods.isConnected())")
        }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 45) { log("45s timeout"); exit(1) }
    spike.start()
    runMainLoop()
}

// MARK: - Mode: serve

/// Mac as a handover participant: holds the cross-device channel and gives the AirPods up when the
/// phone asks (kRequestDisconnect 00 02 00 00, or kRequestHandover 00 02 00 04). Replies with
/// kAirPodsDisconnected (00 01 00 00) once they are released. Run for 3 minutes.
var activeServeSpike: RFCOMMSpike?
var activeServeConnect: ConnectDelegate?

func runServe(phoneAddress: String, airpodsAddress: String) -> Never {
    let phone = deviceFromArgument(phoneAddress)
    let airpods = deviceFromArgument(airpodsAddress)
    let spike = RFCOMMSpike(device: phone)
    activeServeSpike = spike
    spike.onOpen = {
        log("Serving. AirPods isConnected=\(airpods.isConnected()).")
        if airpods.isConnected() {
            spike.send([0x00, 0x01, 0x00, 0x01], label: "kAirPodsConnected")
            log("READY: pause and then play audio on the phone now.")
        } else {
            // Take them first (the forward handover), then wait for the phone to ask for them back.
            spike.send([0x00, 0x02, 0x00, 0x00], label: "kRequestDisconnect")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                let delegate = ConnectDelegate()
                delegate.completion = { status, connected in
                    log("Took the AirPods: status=\(describe(status)) isConnected=\(connected)")
                    if connected {
                        spike.send([0x00, 0x01, 0x00, 0x01], label: "kAirPodsConnected")
                        log("READY: pause and then play audio on the phone now.")
                    }
                }
                activeServeConnect = delegate
                _ = airpods.openConnection(delegate)
            }
        }
    }
    spike.onData = { data in
        let bytes = [UInt8](data)
        if bytes == [0x00, 0x02, 0x00, 0x03] {  // kRequestConnectionStat: answer with the current state
            let state: [UInt8] = airpods.isConnected() ? [0x00, 0x01, 0x00, 0x01] : [0x00, 0x01, 0x00, 0x00]
            spike.send(state, label: "status reply")
            return
        }
        guard bytes == [0x00, 0x02, 0x00, 0x00] || bytes == [0x00, 0x02, 0x00, 0x04] else { return }
        let started = Date()
        log("Phone requested the AirPods (\(bytes == [0x00, 0x02, 0x00, 0x04] ? "handover" : "disconnect"))")
        guard airpods.isConnected() else {
            log("AirPods not connected here; replying disconnected")
            spike.send([0x00, 0x01, 0x00, 0x00], label: "kAirPodsDisconnected")
            return
        }
        let rc = airpods.closeConnection()
        log("closeConnection returned \(describe(rc))")
        spike.send([0x00, 0x01, 0x00, 0x00], label: "kAirPodsDisconnected")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            log("RELEASED after \(String(format: "%.1f", Date().timeIntervalSince(started)))s; AirPods isConnected=\(airpods.isConnected())")
        }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 180) { log("3 min elapsed; closing"); spike.shutdown() }
    spike.start()
    runMainLoop()
}

// MARK: - Mode: airpods-connect

/// Delegate for openConnection(_:). IOBluetooth calls connectionComplete:status: when done.
final class ConnectDelegate: NSObject {
    var completion: ((IOReturn, Bool) -> Void)?
    @objc func connectionComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
        log("connectionComplete: status=\(describe(status)) isConnected=\(device.isConnected())")
        if let completion { completion(status, device.isConnected()); return }
        exit(status == kIOReturnSuccess ? 0 : 1)
    }
}

/// Keeps the connect delegate alive until the callback fires.
var activeConnectDelegate: ConnectDelegate?

func runAirPodsConnect(address: String) -> Never {
    let device = deviceFromArgument(address)
    log("openConnection on \(device.addressString ?? address) (name: \(device.name ?? "?"))")

    let delegate = ConnectDelegate()
    activeConnectDelegate = delegate

    let status = device.openConnection(delegate)
    log("openConnection returned \(describe(status)); isConnected=\(device.isConnected())")
    if status != kIOReturnSuccess {
        exit(1)
    }

    // Safety net in case the completion callback never arrives.
    DispatchQueue.main.asyncAfter(deadline: .now() + 30) {
        log("No connectionComplete after 30s; isConnected=\(device.isConnected())")
        exit(1)
    }

    runMainLoop()
}

// MARK: - Mode: airpods-disconnect

func runAirPodsDisconnect(address: String) -> Never {
    let device = deviceFromArgument(address)
    log("closeConnection on \(device.addressString ?? address) (name: \(device.name ?? "?"))")

    let status = device.closeConnection()
    log("closeConnection returned \(describe(status))")

    // closeConnection is synchronous, but re-check state after a moment to be sure.
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
        log("isConnected after 1s: \(device.isConnected())")
        exit(status == kIOReturnSuccess ? 0 : 1)
    }

    runMainLoop()
}

// MARK: - Entry point

func printUsage() {
    let name = URL(fileURLWithPath: CommandLine.arguments[0]).lastPathComponent
    print("""
    usage:
      \(name) rfcomm <PHONE_MAC>
      \(name) airpods-connect <AIRPODS_MAC>
      \(name) airpods-disconnect <AIRPODS_MAC>
      \(name) paired
      \(name) setup-server
      \(name) serve <PHONE_MAC> <AIRPODS_MAC>
      \(name) handover <PHONE_MAC> <AIRPODS_MAC>
      \(name) setup-client <PHONE_MAC>

    MAC format: AA:BB:CC:DD:EE:FF
    """)
}

let arguments = Array(CommandLine.arguments.dropFirst())

switch (arguments.first, arguments.count) {
case ("paired", 1):
    listPaired()
case ("rfcomm", 2):
    runRFCOMM(address: arguments[1])
case ("setup-client", 2):
    runSetupClient(address: arguments[1])
case ("handover", 3):
    runHandover(phoneAddress: arguments[1], airpodsAddress: arguments[2])
case ("serve", 3):
    runServe(phoneAddress: arguments[1], airpodsAddress: arguments[2])
case ("setup-server", 1):
    runSetupServer()
case ("airpods-connect", 2):
    runAirPodsConnect(address: arguments[1])
case ("airpods-disconnect", 2):
    runAirPodsDisconnect(address: arguments[1])
default:
    printUsage()
    exit(1)
}
