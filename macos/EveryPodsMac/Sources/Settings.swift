// Settings.swift
//
// Which paired devices this Mac coordinates: the Android phone and the AirPods.

import Foundation
import IOBluetooth

enum Settings {
    private static let defaults = UserDefaults.standard

    static var phoneAddress: String? {
        get { defaults.string(forKey: "phoneAddress") }
        set { defaults.set(newValue, forKey: "phoneAddress") }
    }
    static var airPodsAddress: String? {
        get { defaults.string(forKey: "airPodsAddress") }
        set { defaults.set(newValue, forKey: "airPodsAddress") }
    }

    /// Take the AirPods automatically when this Mac starts playing audio (default on).
    static var autoTake: Bool {
        get { defaults.object(forKey: "autoTake") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "autoTake") }
    }

    /// Paired devices, one entry per address, sorted by name.
    static func pairedDevices() -> [IOBluetoothDevice] {
        let devices = (IOBluetoothDevice.pairedDevices() as? [IOBluetoothDevice]) ?? []
        var seen = Set<String>()
        return devices
            .filter { device in device.addressString.map { seen.insert($0).inserted } ?? false }
            .sorted { ($0.name ?? "") < ($1.name ?? "") }
    }
}

/// Normalises "AA:BB:..." or "aa-bb-..." to the lowercase dashed form IOBluetooth uses.
func normalizedAddress(_ raw: String) -> String {
    raw.lowercased().replacingOccurrences(of: ":", with: "-")
}
