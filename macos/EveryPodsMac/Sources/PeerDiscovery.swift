// PeerDiscovery.swift
//
// Finds paired devices that run the EveryPods Android app (they advertise the CrossDevice service), and the
// paired AirPods. Mirrors discoverAndroidPeers / discoverAirPods in windows/src/main.cpp.

import Foundation
import IOBluetooth

struct DiscoveredPeer {
    let address: String
    let name: String
}

enum PeerDiscovery {
    /// Paired phones and computers whose SDP records contain the CrossDevice service. Cached records are checked first; devices without cached records are queried
    /// (only devices classed as phones or computers, so a car kit or headphones are not woken up for nothing).
    static func findPeers(completion: @escaping ([DiscoveredPeer]) -> Void) {
        var found: [DiscoveredPeer] = []
        var toQuery: [IOBluetoothDevice] = []
        let uuid = IOBluetoothSDPUUID(data: Data(ServiceUUID.crossDevice))

        for device in Settings.pairedDevices() {
            guard let address = device.addressString else { continue }
            let cached = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
            if cached.contains(where: { $0.hasService(from: [uuid]) }) {
                found.append(DiscoveredPeer(address: address, name: device.name ?? address))
            } else if device.deviceClassMajor == UInt32(kBluetoothDeviceClassMajorPhone)
                        || device.deviceClassMajor == UInt32(kBluetoothDeviceClassMajorComputer) {
                toQuery.append(device)
            }
        }

        var lookups: [ServiceLookup] = []
        var pending = toQuery.count
        guard pending > 0 else { return completion(found) }
        for device in toQuery {
            let lookup = ServiceLookup(device: device, uuidBytes: ServiceUUID.crossDevice)
            lookups.append(lookup)
            lookup.find(fresh: true) { channel in  // a user-started search: never trust a stale cache
                if channel != nil, let address = device.addressString {
                    found.append(DiscoveredPeer(address: address, name: device.name ?? address))
                }
                pending -= 1
                if pending == 0 {
                    _ = lookups  // keep the lookups alive until the last one reports
                    completion(found)
                }
            }
        }
    }

    /// The paired AirPods (a paired audio device whose name contains "AirPods").
    static func findAirPods() -> [DiscoveredPeer] {
        Settings.pairedDevices().compactMap { device in
            guard let address = device.addressString, let name = device.name, name.contains("AirPods") else { return nil }
            return DiscoveredPeer(address: address, name: name)
        }
    }
}
