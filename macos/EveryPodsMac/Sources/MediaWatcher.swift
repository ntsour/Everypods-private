// MediaWatcher.swift
//
// Watches what this Mac is doing with audio using only public CoreAudio APIs (macOS 14.4+):
//   * playing: some app has an active audio output stream
//   * inCall:  some app has an active audio input stream (microphone), which means a call or recording
// Measured behaviour: playback shows up within ~1s; after a pause the flag lingers ~10-15s
// (browsers keep the stream open), so "started" is a reliable trigger but "stopped" is slow.

import CoreAudio
import Foundation

final class MediaWatcher {
    struct Snapshot: Equatable {
        var playingApps: [String] = []
        var micApps: [String] = []
        var playing: Bool { !playingApps.isEmpty }
        var inCall: Bool { !micApps.isEmpty }
    }

    /// Fires once when audio has been playing for `sustain` seconds (not for short system sounds).
    var onPlaybackStarted: (() -> Void)?
    var onChange: (() -> Void)?
    private(set) var snapshot = Snapshot()

    private let sustain: TimeInterval = 2
    private var timer: Timer?
    private var playingSince: Date?
    private var startFired = false

    /// Processes that are never "playback" or "call": system plumbing and this app.
    private let ignoredPrefixes = [
        "com.apple.HeadphoneSettings", "com.apple.audio.", "com.apple.systemsound",
        "com.apple.coreaudio", "com.everypods.mac",
    ]

    func start() {
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.poll() }
        poll()
    }

    private func poll() {
        let next = readSnapshot()
        if next.playing {
            if playingSince == nil { playingSince = Date() }
            if !startFired, let since = playingSince, Date().timeIntervalSince(since) >= sustain {
                startFired = true
                Log.write("Media: playback started (\(next.playingApps.joined(separator: ", ")))")
                onPlaybackStarted?()
            }
        } else {
            playingSince = nil
            startFired = false
        }
        if next != snapshot {
            snapshot = next
            onChange?()
        }
    }

    private func readSnapshot() -> Snapshot {
        var result = Snapshot()
        for process in Self.objects(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyProcessObjectList) {
            let bundle = Self.string(process, kAudioProcessPropertyBundleID)
            if ignoredPrefixes.contains(where: { bundle.hasPrefix($0) }) { continue }
            if Self.flag(process, kAudioProcessPropertyIsRunningOutput) { result.playingApps.append(bundle) }
            if Self.flag(process, kAudioProcessPropertyIsRunningInput) { result.micApps.append(bundle) }
        }
        return result
    }

    // MARK: CoreAudio helpers

    private static func flag(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> Bool {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        return AudioObjectGetPropertyData(object, &address, 0, nil, &size, &value) == noErr && value != 0
    }

    private static func objects(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> [AudioObjectID] {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(object, &address, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(object, &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }

    private static func string(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(object, &address, 0, nil, &size, &value) == noErr, let value else { return "?" }
        return value.takeRetainedValue() as String
    }
}
