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
