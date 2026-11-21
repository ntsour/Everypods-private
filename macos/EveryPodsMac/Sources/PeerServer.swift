// PeerServer.swift
//
// Listens on the shared "EveryPodsCrossDevice" service so phones and other computers can open a link to this
// Mac (Windows: PeerServer.cpp). Publishes an SDP record with an RFCOMM channel and hands every incoming
// channel to `onChannel`.

import Foundation
import IOBluetooth

final class PeerServer: NSObject {
    var onChannel: ((IOBluetoothRFCOMMChannel, IOBluetoothDevice?) -> Void)?
    private var record: IOBluetoothSDPServiceRecord?
    private var notification: IOBluetoothUserNotification?

    func start() {
        guard record == nil else { return }
        let uuid = IOBluetoothSDPUUID(data: Data(ServiceUUID.crossDevice))
        let dict: [String: Any] = [
            "0001 - ServiceClassIDList": [uuid],
            "0004 - ProtocolDescriptorList": [
                [IOBluetoothSDPUUID(uuid16: 0x0100)],  // L2CAP
                [IOBluetoothSDPUUID(uuid16: 0x0003),   // RFCOMM; IOBluetooth does not assign a channel when it is 0, so a fixed one is used
                 ["DataElementSize": 1, "DataElementType": 1, "DataElementValue": 23]],
            ],
            "0100 - ServiceName*": "EveryPodsCrossDevice",
        ]
        guard let published = IOBluetoothSDPServiceRecord.publishedServiceRecord(with: dict) else {
            return Log.write("PeerServer: could not publish the service record")
        }
        record = published
        var channelID: BluetoothRFCOMMChannelID = 0
        guard published.getRFCOMMChannelID(&channelID) == kIOReturnSuccess else {
            return Log.write("PeerServer: no RFCOMM channel in the published record")
        }
        notification = IOBluetoothRFCOMMChannel.register(
            forChannelOpenNotifications: self,
            selector: #selector(channelOpened(_:channel:)),
            withChannelID: channelID,
            direction: kIOBluetoothUserNotificationChannelDirectionIncoming)
        Log.write("PeerServer: advertising EveryPodsCrossDevice on RFCOMM channel \(channelID)")
    }

    func stop() {
        notification?.unregister()
        notification = nil
        if let record {
            record.remove()
            self.record = nil
        }
    }

    @objc private func channelOpened(_ note: IOBluetoothUserNotification, channel: IOBluetoothRFCOMMChannel) {
        let device = channel.getDevice()
        Log.write("PeerServer: incoming channel from \(device?.addressString ?? "unknown")")
        onChannel?(channel, device)
    }
}
