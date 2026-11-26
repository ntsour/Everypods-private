// TriggerApps.swift
//
// Which apps may move the AirPods to this Mac, and how. Mirrors the Windows "Apps that take or keep the
// AirPods" list (windows/src/TriggerApps.hpp): each listed app has three independent settings, any combination
// allowed:
//
//   takeOnMic  - "Take the AirPods when it opens the mic"
//   takeOnPlay - "Take the AirPods when it starts playing" (no intent or sound check)
//   keepOnMic  - "Keep the AirPods while it uses the mic": its open mic is a call for peers (audioUse=call),
//                so phones and other computers leave the AirPods here, and their requests are refused.
//
// Mac difference: the Mac sees every app's audio, so an app that is not listed still takes the AirPods when the
// user starts it or after sustained sound (MediaWatcher), and its mic plus output is a call. A listed app
// follows exactly its settings, so a listed app with none set never takes the AirPods ("Never take", Mac only).
//
// Apps are keyed by the bundle ID of the app the user knows (Chrome, not its helper process). Pure logic,
// no AppKit: unit-tested in Tests/TriggerAppsTests.swift.

import Foundation

struct TriggerSettings: Equatable {
    var takeOnMic = false
    var takeOnPlay = false
    var keepOnMic = false

    static let never = TriggerSettings()
    var isNever: Bool { self == .never }

    /// Saved form: comma-separated names ("" = never take). The single choices of the earlier version still load:
    /// call = takeOnMic + keepOnMic, keep = keepOnMic, media = takeOnPlay, ignore = never.
    init(saved: String) {
        for part in saved.split(separator: ",").map({ $0.trimmingCharacters(in: .whitespaces).lowercased() }) {
            switch part {
            case "takeonmic": takeOnMic = true
            case "takeonplay": takeOnPlay = true
            case "keeponmic": keepOnMic = true
            case "call": takeOnMic = true; keepOnMic = true
            case "keep": keepOnMic = true
            case "media": takeOnPlay = true
            default: break   // "ignore", unknown: nothing
            }
        }
    }

    init(takeOnMic: Bool = false, takeOnPlay: Bool = false, keepOnMic: Bool = false) {
        self.takeOnMic = takeOnMic
        self.takeOnPlay = takeOnPlay
        self.keepOnMic = keepOnMic
    }

    var saved: String {
        [takeOnMic ? "takeonmic" : nil, takeOnPlay ? "takeonplay" : nil, keepOnMic ? "keeponmic" : nil]
            .compactMap { $0 }.joined(separator: ",")
    }

    /// For the log.
    var text: String {
        if isNever { return "never takes the AirPods" }
        return [takeOnMic ? "takes the AirPods when it opens the mic" : nil,
                takeOnPlay ? "takes the AirPods when it starts playing" : nil,
                keepOnMic ? "keeps the AirPods while it uses the mic" : nil].compactMap { $0 }.joined(separator: "; ")
    }
}

/// The three settings, for menus (names shared with Windows).
enum TriggerSetting: String, CaseIterable {
    case takeOnMic, takeOnPlay, keepOnMic

    var title: String {
        switch self {
        case .takeOnMic: return "Take the AirPods when it opens the mic"
        case .takeOnPlay: return "Take the AirPods when it starts playing"
        case .keepOnMic: return "Keep the AirPods while it uses the mic"
        }
    }

    func get(_ s: TriggerSettings) -> Bool {
        switch self {
        case .takeOnMic: return s.takeOnMic
        case .takeOnPlay: return s.takeOnPlay
        case .keepOnMic: return s.keepOnMic
        }
    }

    func set(_ s: inout TriggerSettings, _ on: Bool) {
        switch self {
        case .takeOnMic: s.takeOnMic = on
        case .takeOnPlay: s.takeOnPlay = on
        case .keepOnMic: s.keepOnMic = on
        }
    }
}

struct TriggerApp: Equatable {
    var bundle: String
    var settings: TriggerSettings
    var builtIn: Bool
}

/// What one audio process contributes, after its app's rule.
struct AudioUseDecision: Equatable {
    /// Its output counts as playback (reported to peers, paused on a handover). Always true for output.
    var playing = false
    /// Its audio counts as a call in progress (reported to peers; they leave the AirPods here).
    var call = false
    /// Its audio may start a takeover.
    var triggers = false
    /// It starts a takeover without the intent or sound check.
    var trusted = false
    /// Its takeover is for a call (an open mic of a "take on mic" app, a meeting app).
    var callTrigger = false
}

struct TriggerAppList {
    /// Native meeting apps (bundle prefixes): all three settings, and audio output alone means a call (the mic is
    /// closed while muted).
    static let builtIn: [TriggerApp] = [
        "us.zoom.xos", "com.microsoft.teams", "com.apple.facetime", "com.cisco.webex", "com.webex", "com.skype",
    ].map { TriggerApp(bundle: $0, settings: TriggerSettings(takeOnMic: true, takeOnPlay: true, keepOnMic: true), builtIn: true) }

    private(set) var user: [TriggerApp] = []

    /// From the saved form (bundle → settings string).
    init(user: [String: String] = [:]) {
        for (bundle, raw) in user.sorted(by: { $0.key < $1.key }) { set(bundle, TriggerSettings(saved: raw)) }
    }

    var saved: [String: String] { Dictionary(uniqueKeysWithValues: user.map { ($0.bundle, $0.settings.saved) }) }

    var all: [TriggerApp] { Self.builtIn + user }

    static func normalized(_ bundle: String) -> String {
        bundle.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    static func isBuiltIn(_ bundle: String) -> Bool {
        let n = normalized(bundle)
        return builtIn.contains { n.hasPrefix($0.bundle) }
    }

    /// The rule for an app: built-ins by prefix, then the user's apps by exact bundle or sub-bundle
    /// ("com.foo.app" also covers "com.foo.app.helper").
    func find(_ bundle: String) -> TriggerApp? {
        let n = Self.normalized(bundle)
        if let app = Self.builtIn.first(where: { n.hasPrefix($0.bundle) }) { return app }
        return user.first { n == $0.bundle || n.hasPrefix($0.bundle + ".") }
    }

    /// Lists the app with these settings (`.never` included). False for an empty name, a built-in, or no change.
    @discardableResult
    mutating func set(_ bundle: String, _ settings: TriggerSettings) -> Bool {
        let n = Self.normalized(bundle)
        guard !n.isEmpty, !Self.isBuiltIn(n) else { return false }
        if let i = user.firstIndex(where: { $0.bundle == n }) {
            if user[i].settings == settings { return false }
            user[i].settings = settings
            return true
        }
        user.append(TriggerApp(bundle: n, settings: settings, builtIn: false))
        return true
    }

    /// Ticks or unticks one setting. Ticking adds an unlisted app; unticking the last one removes the app (back
    /// to the default), as on Windows.
    @discardableResult
    mutating func toggle(_ bundle: String, _ setting: TriggerSetting) -> Bool {
        let n = Self.normalized(bundle)
        guard !Self.isBuiltIn(n) else { return false }
        var s = user.first { $0.bundle == n }?.settings ?? .never
        setting.set(&s, !setting.get(s))
        return s.isNever ? remove(n) : set(n, s)
    }

    @discardableResult
    mutating func remove(_ bundle: String) -> Bool {
        let n = Self.normalized(bundle)
        let before = user.count
        user.removeAll { $0.bundle == n }
        return user.count != before
    }

    /// How a process of `app` using audio counts. `recorder`: a recorder or dictation (its mic is not a call).
    func decide(app: String, output: Bool, input: Bool, recorder: Bool) -> AudioUseDecision {
        var d = AudioUseDecision()
        // A recorder's output while it records is part of the recording, not playback.
        d.playing = output && !(recorder && input)
        guard let rule = find(app) else {
            // Unlisted: microphone plus output at once is a call or meeting (not a recorder); output alone is
            // playback that must pass the intent or sound check.
            d.call = output && input && !recorder
            d.callTrigger = d.call
            d.triggers = d.call || d.playing
            return d
        }
        if rule.builtIn {
            d.call = output || input
            d.callTrigger = d.call
            d.triggers = d.call
            d.trusted = d.call
            return d
        }
        let s = rule.settings
        let micTake = input && s.takeOnMic
        let playTake = d.playing && s.takeOnPlay
        d.call = input && s.keepOnMic
        d.callTrigger = micTake
        d.triggers = micTake || playTake
        d.trusted = micTake || playTake
        return d
    }
}
