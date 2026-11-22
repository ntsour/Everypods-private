// PeerDiscovery.swift
//
// Finds the paired AirPods. Phones are found over Bluetooth LE (PeerGattCentral), not here.

import Foundation
import IOBluetooth

struct DiscoveredPeer {
    let address: String
    let name: String
}

enum PeerDiscovery {
    /// The paired AirPods (a paired audio device whose name contains "AirPods").
    static func findAirPods() -> [DiscoveredPeer] {
        Settings.pairedDevices().compactMap { device in
            guard let address = device.addressString, let name = device.name, name.contains("AirPods") else { return nil }
            return DiscoveredPeer(address: address, name: name)
        }
    }
}
