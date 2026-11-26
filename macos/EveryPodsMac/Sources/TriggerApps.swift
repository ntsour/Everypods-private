// TriggerApps.swift
//
// Which apps may move the AirPods to this Mac, and how. Mirrors the Windows "Apps that take or keep the
// AirPods" list (windows/src/TriggerApps.hpp), with one Mac addition: the Mac sees every app's audio, so an
// unlisted app's playback can take the AirPods (after the intent or sound check in MediaWatcher), and
// "never take" lets the user refuse an app outright.
//
//   call    - its open microphone is a call: takes the AirPods and holds them while the mic stays open.
//   keep    - never takes the AirPods, but while it has the mic open they stay here as in a call
//             (a browser used for meetings).
//   media   - takes the AirPods as soon as it starts playing (no intent or sound check).
//   ignore  - never takes the AirPods (notification-heavy apps).
//
// Apps are keyed by the bundle ID of the app the user knows (Chrome, not its helper process). Pure logic,
// no AppKit: unit-tested in Tests/TriggerAppsTests.swift.

import Foundation

enum TriggerKind: String, CaseIterable {
    case call, keep, media, ignore

    /// For menus and the log.
    var text: String {
        switch self {
        case .call: return "takes the AirPods when it opens the mic"
        case .keep: return "keeps the AirPods while it uses the mic (never takes them)"
        case .media: return "takes the AirPods as soon as it starts playing"
        case .ignore: return "never takes the AirPods"
        }
    }
}

struct TriggerApp: Equatable {
    var bundle: String
    var kind: TriggerKind
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
    /// Its playback starts a takeover without the intent or sound check.
    var trusted = false
}

struct TriggerAppList {
    /// Native meeting apps (bundle prefixes): audio output alone means a call (the mic is closed while muted).
    static let builtIn: [TriggerApp] = [
        "us.zoom.xos", "com.microsoft.teams", "com.apple.facetime", "com.cisco.webex", "com.webex", "com.skype",
    ].map { TriggerApp(bundle: $0, kind: .call, builtIn: true) }

    private(set) var user: [TriggerApp] = []

    init(user: [String: String] = [:]) {
        for (bundle, raw) in user.sorted(by: { $0.key < $1.key }) {
            if let kind = TriggerKind(rawValue: raw) { _ = add(bundle, kind) }
        }
    }

    /// Saved form: bundle → kind.
    var saved: [String: String] { Dictionary(uniqueKeysWithValues: user.map { ($0.bundle, $0.kind.rawValue) }) }

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

    /// Adds the app or changes its kind. False for an empty name, a built-in, or no change.
    @discardableResult
    mutating func add(_ bundle: String, _ kind: TriggerKind) -> Bool {
        let n = Self.normalized(bundle)
        guard !n.isEmpty, !Self.isBuiltIn(n) else { return false }
        if let i = user.firstIndex(where: { $0.bundle == n }) {
            if user[i].kind == kind { return false }
            user[i].kind = kind
            return true
        }
        user.append(TriggerApp(bundle: n, kind: kind, builtIn: false))
        return true
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
        let rule = find(app)
        // A recorder's output while it records is part of the recording, not playback.
        d.playing = output && !(recorder && input)
        switch rule?.kind {
        case .call?:
            if rule!.builtIn {
                d.call = output || input
            } else {
                d.call = input      // user call apps: only the mic (as on Windows)
            }
            d.triggers = d.call || d.playing
        case .keep?:
            d.call = input
            d.triggers = false
        case .media?:
            d.call = false
            d.triggers = d.playing
            d.trusted = d.playing
        case .ignore?:
            d.call = false
            d.triggers = false
        case nil:
            // Unlisted: microphone plus output at once is a call or meeting (not a recorder); output alone is
            // playback that must pass the intent or sound check.
            d.call = output && input && !recorder
            d.triggers = d.call || d.playing
        }
        return d
    }
}
