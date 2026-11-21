// Protocol.swift
//
// Cross-device wire protocol shared with the Android app (CrossDevice.kt) and the Windows client
// (windows/common/crossdevice_protocol.hpp). Packets are 4 bytes; AirPods data frames start with
// 04 00 04 00 and are ignored by this client for now.

import Foundation

enum Packet {
    static let airPodsConnected: [UInt8] = [0x00, 0x01, 0x00, 0x01]
    static let airPodsDisconnected: [UInt8] = [0x00, 0x01, 0x00, 0x00]
    static let requestDisconnect: [UInt8] = [0x00, 0x02, 0x00, 0x00]
    static let requestConnectionStatus: [UInt8] = [0x00, 0x02, 0x00, 0x03]
    static let requestHandover: [UInt8] = [0x00, 0x02, 0x00, 0x04]
    static let peerAudioActive: [UInt8] = [0x00, 0x03, 0x00, 0x01]
    static let peerAudioIdle: [UInt8] = [0x00, 0x03, 0x00, 0x00]
}

enum Incoming {
    case airPodsConnected, airPodsDisconnected
    case requestDisconnect, requestHandover, requestConnectionStatus
    case peerAudioActive, peerAudioIdle
    case other

    init(_ data: Data) {
        switch [UInt8](data) {
        case Packet.airPodsConnected: self = .airPodsConnected
        case Packet.airPodsDisconnected: self = .airPodsDisconnected
        case Packet.requestDisconnect: self = .requestDisconnect
        case Packet.requestHandover: self = .requestHandover
        case Packet.requestConnectionStatus: self = .requestConnectionStatus
        case Packet.peerAudioActive: self = .peerAudioActive
        case Packet.peerAudioIdle: self = .peerAudioIdle
        default: self = .other
        }
    }
}

/// SDP service UUIDs (raw bytes) published by the Android app.
enum ServiceUUID {
    /// "EveryPodsCrossDevice": coordination channel carrying the 4-byte packets.
    static let crossDevice: [UInt8] = [
        0x1a, 0xbb, 0xb9, 0xa4, 0x10, 0xe4, 0x40, 0x00,
        0xa7, 0x5c, 0x89, 0x53, 0xc5, 0x47, 0x13, 0x42,
    ]
    /// "EveryPodsBTHandoverSetup": one-off authorization exchange (BtHandoverSync.kt).
    static let handoverSetup: [UInt8] = [
        0xde, 0x51, 0xa5, 0xb3, 0x41, 0x40, 0x4f, 0xb7,
        0xa5, 0x24, 0x96, 0x73, 0x79, 0x18, 0x17, 0x73,
    ]
}
