// UserIntentWatcher.swift
//
// Reads the signals UserIntent decides on: the time since the last left click or trackpad tap, the frontmost
// app, and the "now playing" broadcasts of players that send one (Music, Spotify). Public APIs, no permissions.

import AppKit
import CoreGraphics

final class UserIntentWatcher {
    /// Player bundle → when it last broadcast "Playing".
    private(set) var playerEvents: [String: Date] = [:]
    /// A player just broadcast "Playing" (MediaWatcher re-checks a start that is waiting).
    var onPlayerPlaying: (() -> Void)?

    /// Players that broadcast their state system-wide, with the bundle of the app that plays.
    private let players = [
        ("com.apple.Music.playerInfo", "com.apple.Music"),
        ("com.apple.iTunes.playerInfo", "com.apple.Music"),
        ("com.spotify.client.PlaybackStateChanged", "com.spotify.client"),
    ]

    func start() {
        let center = DistributedNotificationCenter.default()
        for (name, bundle) in players {
            center.addObserver(forName: Notification.Name(name), object: nil, queue: .main) { [weak self] note in
                guard let self, (note.userInfo?["Player State"] as? String) == "Playing" else { return }
                self.playerEvents[bundle] = Date()
                self.onPlayerPlaying?()
            }
        }
    }

    var secondsSinceClick: TimeInterval {
        CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .leftMouseDown)
    }

    var frontmost: String? { NSWorkspace.shared.frontmostApplication?.bundleIdentifier }

    /// Bundle of a process's parent (helpers without a bundle of their own are named after it).
    static func parentBundle(of pid: pid_t) -> String? {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.size
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0, size > 0 else { return nil }
        let parent = info.kp_eproc.e_ppid
        guard parent > 1 else { return nil }
        return NSRunningApplication(processIdentifier: parent)?.bundleIdentifier
    }

    /// The name the user knows for an app bundle (for menus).
    static func displayName(_ bundle: String) -> String {
        if let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundle) {
            return FileManager.default.displayName(atPath: url.path).replacingOccurrences(of: ".app", with: "")
        }
        return bundle
    }
}
