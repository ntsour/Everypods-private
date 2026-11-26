// UserIntent.swift
//
// Did the user ask for the audio that just started? A takeover the user asked for should be fast; audio nobody
// asked for (a notification ding, autoplay) has to prove itself with sustained sound first (MediaWatcher).
//
// Signals, all public and without extra permissions:
//   * a left click or trackpad tap (tap-to-click is the same event) shortly before the audio, while the app that
//     plays is the frontmost app: clicking Play on YouTube or in Spotify. Typing is not counted: a reply's ding
//     right after typing in a chat would look the same.
//   * a player's own "now playing" broadcast (Music, Spotify): the exact play event.
//
// Pure logic here (unit-tested in Tests/UserIntentTests.swift); UserIntentWatcher reads the system.

import Foundation

enum UserIntent {
    /// A click this long before the audio is first seen still counts as asking for it.
    static let clickWindow: TimeInterval = 2
    /// A player's "Playing" broadcast this close to the audio (either side) counts.
    static let playerEventWindow: TimeInterval = 3

    /// The app the user knows for an audio process: Chrome for "com.google.Chrome.helper", Safari for WebKit's
    /// audio processes. `parentBundle` (the parent process's bundle) names processes without a bundle of their own.
    static func owner(processBundle: String, parentBundle: String?) -> String {
        if processBundle.hasPrefix("com.apple.WebKit") { return "com.apple.Safari" }
        for suffix in [".helper.renderer", ".helper.plugin", ".helper.gpu", ".helper"] where processBundle.hasSuffix(suffix) {
            return String(processBundle.dropLast(suffix.count))
        }
        if processBundle.isEmpty || processBundle == "?", let parent = parentBundle, !parent.isEmpty { return parent }
        return processBundle
    }

    /// Whether the user's click asked for this audio: a click within `clickWindow` before the audio was seen,
    /// with the playing app frontmost. `owners` are the apps playing.
    static func clickAsked(secondsSinceClick: TimeInterval, frontmost: String?, owners: [String]) -> Bool {
        guard secondsSinceClick <= clickWindow, let front = frontmost?.lowercased() else { return false }
        return owners.contains { owner in
            let o = owner.lowercased()
            return o == front || o.hasPrefix(front + ".") || front.hasPrefix(o + ".")
        }
    }

    /// Whether a player's "Playing" broadcast (bundle → time) matches the audio seen at `seenAt`.
    static func playerAsked(events: [String: Date], owners: [String], seenAt: Date) -> Bool {
        owners.contains { owner in
            let o = owner.lowercased()
            return events.contains { bundle, at in
                (o == bundle.lowercased() || o.hasPrefix(bundle.lowercased() + "."))
                    && abs(at.timeIntervalSince(seenAt)) <= playerEventWindow
            }
        }
    }
}
