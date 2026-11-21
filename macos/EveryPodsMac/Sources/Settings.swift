// Settings.swift
//
// Which paired devices this Mac coordinates (the phones and the AirPods) and the user's toggles.
// Mirrors windows/src/SettingsStore.

import Foundation
import IOBluetooth

enum Settings {
    private static let defaults = UserDefaults.standard

    /// Addresses of every peer coordinated with (phones and computers alike), in the order they were added.
    static var peers: [String] {
        get {
            var list = defaults.stringArray(forKey: "peers") ?? []
            // Older versions stored phones and computers in separate lists, or a single phone.
            if list.isEmpty {
                list = (defaults.stringArray(forKey: "phones") ?? []) + (defaults.stringArray(forKey: "computers") ?? [])
                if list.isEmpty, let legacy = defaults.string(forKey: "phoneAddress") { list = [legacy] }
            }
            return list
        }
        set { defaults.set(newValue, forKey: "peers") }
    }

    static var airPodsAddress: String? {
        get { defaults.string(forKey: "airPodsAddress") }
        set { defaults.set(newValue, forKey: "airPodsAddress") }
    }

    /// "Take over when media starts" (default on). Reads the older `autoTake` key until it is first set.
    static var takeOnMediaStart: Bool {
        get { defaults.object(forKey: "takeOnMediaStart") as? Bool ?? defaults.object(forKey: "autoTake") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "takeOnMediaStart") }
    }

    /// "Take over when receiving a call" (default on).
    static var takeOnCall: Bool {
        get { defaults.object(forKey: "takeOnCall") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "takeOnCall") }
    }

    /// Set the AirPods as the default audio output when this Mac gets them (default on).
    static var autoRoute: Bool {
        get { defaults.object(forKey: "autoRoute") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "autoRoute") }
    }

    /// Pause what is playing when handing the AirPods to a phone, so audio does not jump to the speakers.
    /// The Windows client never touches playback; on the Mac this is tested and on by default.
    static var pauseOnHandover: Bool {
        get { defaults.object(forKey: "pauseOnHandover") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "pauseOnHandover") }
    }

    /// Set once the first-run discovery has run, so it is not repeated every launch.
    static var didFirstRun: Bool {
        get { defaults.bool(forKey: "didFirstRun") }
        set { defaults.set(newValue, forKey: "didFirstRun") }
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
