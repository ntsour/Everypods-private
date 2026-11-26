// MediaWatcher.swift
//
// Watches what this Mac is doing with audio using only public CoreAudio APIs (macOS 14.4+):
//   * playing: some app has an active audio output stream
//   * inCall:  some app has an active audio input stream (microphone), which means a call or recording
//     A browser meeting counts through its microphone: Meet keeps the mic stream open while muted, and
//     releases it when you leave. An open meeting tab alone is not a call (it stays open after hanging up).
// Measured behaviour: playback shows up within ~1s; after a pause the flag lingers ~10-15s
// (browsers keep the stream open), so "started" is a reliable trigger but "stopped" is slow.

import CoreAudio
import Foundation

final class MediaWatcher {
    struct Snapshot: Equatable {
        var playingApps: [String] = []
        var micApps: [String] = []
        /// Apps with both microphone input and audio output at once: a call or meeting, not a recording.
        var callApps: [String] = []
        /// Core Audio process objects behind `playingApps` (for a level probe).
        var playingProcesses: [AudioObjectID] = []
        var playing: Bool { !playingApps.isEmpty }
        var inCall: Bool { !callApps.isEmpty }
        var micInUse: Bool { !micApps.isEmpty }
    }

    /// Fires once when audio has been playing for `sustain` seconds (not for short system sounds).
    var onPlaybackStarted: (() -> Void)?
    /// Fires when audio stops (late: browsers keep the stream open for 10-15 s after a pause).
    var onPlaybackStopped: (() -> Void)?
    var onChange: (() -> Void)?
    /// Fires the moment audio output is first seen (before `sustain`), so a playback hold can mute at once.
    var onPlaybackSeen: (() -> Void)?
    /// With a playback hold on, the takeover fires after a short sustain: the speakers are already muted, and a
    /// longer wait would only delay the AirPods (short sounds still end well before this).
    var fastStart: () -> Bool = { false }
    private(set) var snapshot = Snapshot()

    private var sustain: TimeInterval { fastStart() ? 0.6 : 2 }
    /// Processes with a running-output listener installed (Core Audio events make detection near-instant).
    private var watchedProcesses = Set<AudioObjectID>()
    private var timer: Timer?
    private var playingSince: Date?
    private var startFired = false

    /// Processes that are never "playback" or "call": system plumbing and this app.
    private let ignoredPrefixes = [
        "com.apple.HeadphoneSettings", "com.apple.audio.", "com.apple.systemsound",
        "com.apple.coreaudio", "com.everypods.mac",
    ]

    /// Recorders and dictation hold an output stream while using the microphone (Voice Memos shows
    /// both flags). They are a microphone in use, not a call and not playback.
    private let recorderPrefixes = [
        "com.apple.VoiceMemos", "com.apple.QuickTimePlayerX", "com.apple.garageband", "com.apple.logic",
        "com.apple.dictation", "com.apple.speech", "com.apple.siri", "com.apple.assistant",
    ]

    /// Native meeting apps: audio output alone means a call (the mic is closed while muted).
    private let callAppPrefixes = [
        "us.zoom.xos", "com.microsoft.teams", "com.apple.FaceTime", "com.cisco.webex", "com.webex",
        "com.skype", "com.apple.facetime",
    ]

    func start() {
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.poll() }
        watchProcessList()
        poll()
    }

    /// Re-polls whenever a process starts or stops output (or the process list changes), instead of waiting
    /// for the next 1 s tick. The tick stays as a safety net.
    private func watchProcessList() {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyProcessObjectList,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, DispatchQueue.main) { [weak self] _, _ in
            self?.watchProcesses()
            self?.poll()
        }
        watchProcesses()
    }

    private func watchProcesses() {
        for process in Self.objects(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyProcessObjectList)
        where !watchedProcesses.contains(process) {
            var address = AudioObjectPropertyAddress(mSelector: kAudioProcessPropertyIsRunningOutput,
                                                     mScope: kAudioObjectPropertyScopeGlobal,
                                                     mElement: kAudioObjectPropertyElementMain)
            let status = AudioObjectAddPropertyListenerBlock(process, &address, DispatchQueue.main) { [weak self] _, _ in
                self?.poll()
            }
            if status == noErr { watchedProcesses.insert(process) }
        }
    }

    /// The app currently producing audio ("" if none); used to recognise the same app re-triggering.
    var currentAppID: String { snapshot.playingApps.first ?? "" }

    private func poll() {
        let next = readSnapshot()
        if next.playing {
            if playingSince == nil {
                playingSince = Date()
                if !startFired { onPlaybackSeen?() }
                // Check again right when the sustain elapses rather than on the next 1 s tick.
                DispatchQueue.main.asyncAfter(deadline: .now() + sustain + 0.05) { [weak self] in self?.poll() }
            }
            if !startFired, let since = playingSince, Date().timeIntervalSince(since) >= sustain {
                startFired = true
                Log.write("Media: playback started (\(next.playingApps.joined(separator: ", ")))")
                onPlaybackStarted?()
            }
        } else {
            playingSince = nil
            if startFired { onPlaybackStopped?() }
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
            let output = Self.flag(process, kAudioProcessPropertyIsRunningOutput)
            let input = Self.flag(process, kAudioProcessPropertyIsRunningInput)
            let recorder = recorderPrefixes.contains(where: { bundle.hasPrefix($0) })
            if input { result.micApps.append(bundle) }
            if output && !(recorder && input) {
                result.playingApps.append(bundle)
                result.playingProcesses.append(process)
            }
            if output && input && !recorder { result.callApps.append(bundle) }
            else if output && callAppPrefixes.contains(where: { bundle.hasPrefix($0) }) { result.callApps.append(bundle) }
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
