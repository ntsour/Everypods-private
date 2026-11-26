// TriggerApps.swift
//
// Which apps may move the AirPods to this Mac, and how. Every app, built in or not, has the same three settings,
// in any combination (the same names as on Windows, windows/src/TriggerApps.hpp):
//
//   takeOnMic  - "Take the AirPods when it opens the mic": an open mic takes them at once.
//   takeOnPlay - "Take the AirPods when it starts playing": real playback takes them. Notification dings are
//                filtered: the start needs a click in the app, a player's "Playing" broadcast, or 1 s of sound
//                (MediaWatcher).
//   keepOnMic  - "Keep the AirPods while it uses the mic": its open mic is a call for peers (audioUse=call), so
//                phones and other computers leave the AirPods here, and their requests are refused.
//
// Nothing ticked: the app never takes or keeps the AirPods.
//
// Where an app's settings come from, first match wins:
//   1. what the user set (saved, any app);
//   2. the built-in catalog below (meeting apps and browsers: all three; media players: play), shown in the menu even before they
//      make a sound, with "Reset to built-in" once changed;
//   3. an app seen using the mic and sound together (a call) starts with all three ticked;
//   4. any other app starts with "Take the AirPods when it starts playing".
//
// Apps are keyed by the bundle ID of the app the user knows (Chrome, not its helper process). Pure logic,
// no AppKit: unit-tested in Tests/TriggerAppsTests.swift.

import Foundation

struct TriggerSettings: Equatable {
    var takeOnMic = false
    var takeOnPlay = false
    var keepOnMic = false

    static let none = TriggerSettings()
    static let play = TriggerSettings(takeOnPlay: true)
    static let all = TriggerSettings(takeOnMic: true, takeOnPlay: true, keepOnMic: true)

    init(takeOnMic: Bool = false, takeOnPlay: Bool = false, keepOnMic: Bool = false) {
        self.takeOnMic = takeOnMic
        self.takeOnPlay = takeOnPlay
        self.keepOnMic = keepOnMic
    }

    /// Saved form: comma-separated names ("" = nothing ticked). Values saved by earlier versions still load:
    /// call = takeOnMic + keepOnMic, keep = keepOnMic, media = takeOnPlay, ignore = nothing.
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

    var saved: String {
        [takeOnMic ? "takeonmic" : nil, takeOnPlay ? "takeonplay" : nil, keepOnMic ? "keeponmic" : nil]
            .compactMap { $0 }.joined(separator: ",")
    }

    /// For the menu and the log.
    var text: String {
        if self == .none { return "never takes or keeps the AirPods" }
        return [takeOnMic ? "takes the AirPods when it opens the mic" : nil,
                takeOnPlay ? "takes the AirPods when it starts playing" : nil,
                keepOnMic ? "keeps the AirPods while it uses the mic" : nil].compactMap { $0 }.joined(separator: "; ")
    }
}

/// The three settings, for menus.
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

struct BuiltInApp: Equatable {
    /// Bundle ID prefix (lower case); also the key the user's changes are saved under.
    var bundle: String
    var name: String
    var settings: TriggerSettings
    /// A meeting app: its sound alone counts as using the mic (a meeting joined muted closes the mic).
    var meeting = false
}

/// What one audio process contributes, after its app's settings.
struct AudioUseDecision: Equatable {
    /// Its output counts as playback (reported to peers, paused on a handover).
    var playing = false
    /// Its audio counts as a call in progress (reported to peers; they leave the AirPods here).
    var call = false
    /// Its audio may start a takeover.
    var triggers = false
    /// It starts a takeover at once (an open mic), without the playback filter.
    var trusted = false
}

struct TriggerAppList {
    static let builtIn: [BuiltInApp] = [
        BuiltInApp(bundle: "us.zoom.xos", name: "Zoom", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.microsoft.teams", name: "Microsoft Teams", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.apple.facetime", name: "FaceTime", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.cisco.webex", name: "Webex", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.webex", name: "Webex Meetings", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.skype", name: "Skype", settings: .all, meeting: true),
        BuiltInApp(bundle: "com.google.chrome", name: "Google Chrome", settings: .all),
        BuiltInApp(bundle: "com.apple.safari", name: "Safari", settings: .all),
        BuiltInApp(bundle: "com.microsoft.edgemac", name: "Microsoft Edge", settings: .all),
        BuiltInApp(bundle: "com.brave.browser", name: "Brave Browser", settings: .all),
        BuiltInApp(bundle: "company.thebrowser.browser", name: "Arc", settings: .all),
        BuiltInApp(bundle: "org.mozilla.firefox", name: "Firefox", settings: .all),
        BuiltInApp(bundle: "com.apple.music", name: "Music", settings: .play),
        BuiltInApp(bundle: "com.spotify.client", name: "Spotify", settings: .play),
    ]

    /// What the user set: app key → settings.
    private(set) var user: [String: TriggerSettings] = [:]
    /// Apps seen using the mic and sound together (calls): they start with all three settings.
    private(set) var callSeen: Set<String> = []

    /// From the saved forms (app key → settings string, and the call-seen keys).
    init(user: [String: String] = [:], callSeen: [String] = []) {
        for (bundle, raw) in user {
            let k = Self.normalized(bundle)
            if !k.isEmpty { self.user[k] = TriggerSettings(saved: raw) }
        }
        self.callSeen = Set(callSeen.map(Self.normalized))
    }

    var savedUser: [String: String] { user.mapValues(\.saved) }

    static func normalized(_ bundle: String) -> String {
        bundle.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    static func builtIn(_ bundle: String) -> BuiltInApp? {
        let n = normalized(bundle)
        return builtIn.first { b in
            // Meeting apps match by plain prefix as before (com.cisco.webexmeetingsapp); others by bundle, a
            // sub-bundle, or a version suffix (com.microsoft.teams2).
            n == b.bundle || n.hasPrefix(b.bundle + ".") || (n.hasPrefix(b.bundle) && (b.meeting || n.dropFirst(b.bundle.count).allSatisfy(\.isNumber)))
        }
    }

    /// The key an app's settings live under: its built-in entry, an app the user set (sub-bundles such as
    /// "com.foo.app.helper" follow "com.foo.app"), or its own bundle.
    func key(_ bundle: String) -> String {
        let n = Self.normalized(bundle)
        if let b = Self.builtIn(n) { return b.bundle }
        if let set = user.keys.first(where: { n.hasPrefix($0 + ".") }) { return set }
        return n
    }

    func settings(_ bundle: String) -> TriggerSettings {
        let k = key(bundle)
        if let s = user[k] { return s }
        if let b = Self.builtIn(k) { return b.settings }
        return callSeen.contains(k) ? .all : .play
    }

    /// A built-in app the user changed (offers "Reset to built-in").
    func isChangedBuiltIn(_ bundle: String) -> Bool {
        let k = key(bundle)
        guard let b = Self.builtIn(k), let s = user[k] else { return false }
        return s != b.settings
    }

    /// Ticks or unticks one setting; the app keeps exactly what is ticked (nothing ticked: never takes or keeps).
    @discardableResult
    mutating func toggle(_ bundle: String, _ setting: TriggerSetting) -> Bool {
        let k = key(bundle)
        guard !k.isEmpty, k != "?" else { return false }
        var s = settings(k)
        setting.set(&s, !setting.get(s))
        user[k] = s
        return true
    }

    /// Back to the built-in settings.
    @discardableResult
    mutating func reset(_ bundle: String) -> Bool { user.removeValue(forKey: key(bundle)) != nil }

    /// Notes an app using the mic and sound together. True when that newly gives it all three settings.
    @discardableResult
    mutating func noteCall(_ bundle: String) -> Bool {
        let k = key(bundle)
        guard user[k] == nil, Self.builtIn(k) == nil, !k.isEmpty, k != "?" else { return false }
        return callSeen.insert(k).inserted
    }

    /// How a process of `app` using audio counts. `recorder`: a recorder or dictation (its mic is not a call).
    func decide(app: String, output: Bool, input: Bool, recorder: Bool) -> AudioUseDecision {
        var d = AudioUseDecision()
        let s = settings(app)
        let meeting = Self.builtIn(key(app))?.meeting == true
        // A recorder's output while it records is part of the recording, not playback; its mic is no call.
        d.playing = output && !(recorder && input)
        let micUse = !recorder && (input || (meeting && output))
        d.call = micUse && s.keepOnMic
        let micTake = micUse && s.takeOnMic
        d.trusted = micTake
        d.triggers = micTake || (d.playing && s.takeOnPlay)
        return d
    }
}
