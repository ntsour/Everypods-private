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

    /// This Mac's EveryPods installation ID (sent in the Bluetooth LE HELLO), created on first use.
    static var installationId: String {
        if let id = defaults.string(forKey: "installationId"), UUID(uuidString: id) != nil { return id }
        let id = UUID().uuidString.lowercased()
        defaults.set(id, forKey: "installationId")
        return id
    }

    /// Phones matched over Bluetooth LE: installation ID → the address the phone is configured under. CoreBluetooth
    /// exposes no addresses, so the first match is by Bluetooth name and is then remembered.
    static var gattPeerAddresses: [String: String] {
        get { defaults.dictionary(forKey: "gattPeerAddresses") as? [String: String] ?? [:] }
        set { defaults.set(newValue, forKey: "gattPeerAddresses") }
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

    /// "Hold audio until AirPods connect" (default off): mute the speakers and pause when a media takeover starts,
    /// resume once the AirPods are the output, and show the progress panel. Mirrors the Android setting.
    static var holdPlayback: Bool {
        get { defaults.bool(forKey: "holdPlayback") }
        set { defaults.set(newValue, forKey: "holdPlayback") }
    }

    /// Every app's "Apps that take or keep the AirPods" settings: what the user set (bundle → settings) and the
    /// apps seen in a call. Read on every audio poll, so the parsed list is cached until it changes.
    static var triggerApps: TriggerAppList {
        get {
            let raw = defaults.dictionary(forKey: "triggerApps") as? [String: String] ?? [:]
            let calls = defaults.stringArray(forKey: "callSeenApps") ?? []
            if raw != cachedTriggerRaw || calls != cachedCallSeen {
                cachedTriggerRaw = raw
                cachedCallSeen = calls
                cachedTriggerApps = TriggerAppList(user: raw, callSeen: calls)
            }
            return cachedTriggerApps
        }
        set {
            defaults.set(newValue.savedUser, forKey: "triggerApps")
            defaults.set(newValue.callSeen.sorted(), forKey: "callSeenApps")
        }
    }
    private static var cachedTriggerRaw: [String: String]?
    private static var cachedCallSeen: [String]?
    private static var cachedTriggerApps = TriggerAppList()

    /// Set once the first-run discovery has run, so it is not repeated every launch.
    /// "Connect AirPods when the case opens (if this Mac used them last)" (default on).
    static var lidOpenConnect: Bool {
        get { defaults.object(forKey: "lidOpenConnect") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "lidOpenConnect") }
    }

    /// The lid-open lease: this Mac was the last device to use the AirPods (default off).
    static var lidLease: Bool {
        get { defaults.bool(forKey: "lidLease") }
        set { defaults.set(newValue, forKey: "lidLease") }
    }

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
