// MediaWatcher.swift
//
// Watches what this Mac is doing with audio using only public CoreAudio APIs (macOS 14.4+):
//   * playing: some app has an active audio output stream
//   * inCall:  some app has an active audio input stream (microphone), which means a call or recording
// Measured behaviour: playback shows up within ~1s; after a pause the flag lingers ~10-15s
// (browsers keep the stream open), so "started" is a reliable trigger but "stopped" is slow.
//
// An open output stream is not proof of playback: a browser opens one for a notification ding and keeps it
// running 10-15 s, long past the sustain. So a start that could move the AirPods here (`levelGate`) must be
// one of:
//   * a call (microphone plus output, a meeting app, a "call" app; TriggerApps),
//   * an app the user marked "takes the AirPods as soon as it starts playing",
//   * audio the user asked for (UserIntent: a click or tap in the playing app just before, or a player's
//     "Playing" broadcast),
//   * or sustained sound: about 1 s of real audio, measured by a process tap on the playing processes
//     (PlaybackLevelProbe, macOS 14.2+). Without the tap (older macOS, tap failure, or the capture permission
//     never seen working) the start fires on the open stream as before.
// Apps marked "never takes" or "keeps" never start a takeover.

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
        /// Apps (as the user knows them, e.g. Chrome) whose audio may start a takeover, and their processes.
        var triggerOwners: [String] = []
        var triggerProcesses: [AudioObjectID] = []
        /// A call whose app may start a takeover (not a "keep" app).
        var callTriggers = false
        /// A "takes the AirPods as soon as it starts playing" app is playing.
        var trustedPlaying = false
        var playing: Bool { !playingApps.isEmpty }
        var inCall: Bool { !callApps.isEmpty }
        var micInUse: Bool { !micApps.isEmpty }
        var triggering: Bool { !triggerOwners.isEmpty }
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
    /// True when a start would take the AirPods from another device: the start then needs real sound too.
    var levelGate: () -> Bool = { false }
    /// The user's app rules (Settings.triggerApps).
    var apps: () -> TriggerAppList = { TriggerAppList() }
    let intent = UserIntentWatcher()
    /// Apps that ever used audio (owner bundles), newest last, for the apps menu. Remembered across launches so
    /// the menu lists them before they play again.
    private(set) var seenApps: [String] = UserDefaults.standard.stringArray(forKey: "seenAudioApps") ?? []
    private let seenLimit = 40
    private(set) var snapshot = Snapshot()

    private var sustain: TimeInterval { fastStart() ? 0.6 : 2 }
    /// Processes with a running-output listener installed (Core Audio events make detection near-instant).
    private var watchedProcesses = Set<AudioObjectID>()
    private var timer: Timer?
    private var playingSince: Date?
    private var startFired = false

    /// The sound check for the current stream (see the header). `nil` when none runs.
    private var gate: AnyObject?
    private var gateProcesses = Set<AudioObjectID>()
    private var gateState: GateState = .off
    private var gateLoggedWaiting = false
    /// The user clicked in the playing app just before this stream was seen.
    private var clickIntent = false
    private enum GateState { case off, listening, sustained, failed, gaveUp }
    /// A stream that stays open this long without real sound is left alone (no tap held forever).
    private let gateLimit: TimeInterval = 60
    /// With the capture permission never seen working, all-zero samples this long past the sustain mean
    /// "no permission" rather than "silence", and the start fires as before.
    private let unconfirmedGrace: TimeInterval = 1.5

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

    /// Chromium browsers, scripted to look for an open meeting tab when one is playing audio.
    private let browsers: [(bundlePrefix: String, appName: String)] = [
        ("com.google.Chrome", "Google Chrome"), ("com.brave.Browser", "Brave Browser"),
        ("com.microsoft.edgemac", "Microsoft Edge"), ("company.thebrowser.Browser", "Arc"),
    ]
    private let meetingURL = try! NSRegularExpression(
        pattern: #"https://(meet\.google\.com/[a-z]{3}-[a-z]{4}-[a-z]{3}|[a-z0-9.-]*zoom\.us/(wc|j)/|whereby\.com/[^/?]+|teams\.(microsoft|live)\.com/.*(meetup-join|meet))"#)
    private var meetingTabs: [String: Bool] = [:]
    private var lastTabCheck: [String: Date] = [:]
    private let scriptQueue = DispatchQueue(label: "everypods.tabs", qos: .utility)

    func start() {
        intent.onPlayerPlaying = { [weak self] in self?.poll() }
        intent.start()
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
        if next.triggering {
            if playingSince == nil {
                playingSince = Date()
                if !startFired {
                    clickIntent = UserIntent.clickAsked(secondsSinceClick: intent.secondsSinceClick,
                                                        frontmost: intent.frontmost, owners: next.triggerOwners)
                    if clickIntent { Log.write("Media: \(next.triggerOwners.joined(separator: ", ")) started right after a click in it") }
                    onPlaybackSeen?()
                    if levelGate() && !next.callTriggers && !next.trustedPlaying && !clickIntent && !playerIntent(next) {
                        startGate(next.triggerProcesses)
                    }
                }
                // Check again right when the sustain elapses rather than on the next 1 s tick.
                DispatchQueue.main.asyncAfter(deadline: .now() + sustain + 0.05) { [weak self] in self?.poll() }
                DispatchQueue.main.asyncAfter(deadline: .now() + sustain + unconfirmedGrace + 0.05) { [weak self] in self?.poll() }
            } else if gateState == .listening, !Set(next.triggerProcesses).isSubset(of: gateProcesses) {
                // Another app started output while we listen: listen to all of them.
                startGate(next.triggerProcesses)
            }
            if !startFired, let since = playingSince {
                let elapsed = Date().timeIntervalSince(since)
                if elapsed >= sustain && startAllowed(next, elapsed: elapsed) {
                    startFired = true
                    stopGate()
                    Log.write("Media: playback started (\(next.playingApps.joined(separator: ", ")))")
                    onPlaybackStarted?()
                } else if gateState == .listening && elapsed >= gateLimit {
                    Log.write("Media: no sustained sound from \(next.triggerOwners.joined(separator: ", ")) in \(Int(gateLimit)) s; not a start")
                    stopGate()
                    gateState = .gaveUp   // this stream is done; a new stream gets a new check
                }
            }
        } else {
            playingSince = nil
            clickIntent = false
            stopGate()
            gateState = .off
            if startFired { onPlaybackStopped?() }
            startFired = false
        }
        if next != snapshot {
            snapshot = next
            onChange?()
        }
    }

    private func playerIntent(_ next: Snapshot) -> Bool {
        UserIntent.playerAsked(events: intent.playerEvents, owners: next.triggerOwners, seenAt: playingSince ?? Date())
    }

    // MARK: Sound check

    /// Whether an open stream past its sustain counts as a start.
    private func startAllowed(_ next: Snapshot, elapsed: TimeInterval) -> Bool {
        if next.callTriggers { return true }   // calls and meeting apps: as before
        if next.trustedPlaying { return true }
        if clickIntent { return true }
        if playerIntent(next) {
            if gateState == .listening { Log.write("Media: \(next.triggerOwners.joined(separator: ", ")) announced playback") }
            return true
        }
        switch gateState {
        case .off, .sustained, .failed:
            return true
        case .gaveUp:
            return false
        case .listening:
            if #available(macOS 14.2, *), !PlaybackLevelProbe.captureConfirmed, elapsed >= sustain + unconfirmedGrace,
               let probe = gate as? PlaybackLevelProbe, !probe.receivedAudio {
                Log.write("Media: sound check heard nothing and capture was never seen working; starting on the open stream")
                return true
            }
            if !gateLoggedWaiting {
                gateLoggedWaiting = true
                Log.write("Media: output from \(next.playingApps.joined(separator: ", ")) but no sustained sound yet (a notification?); waiting")
            }
            return false
        }
    }

    private func startGate(_ processes: [AudioObjectID]) {
        stopGate()
        guard #available(macOS 14.2, *), !processes.isEmpty else { return }
        let probe = PlaybackLevelProbe(detector: SustainedSoundDetector())
        gate = probe
        gateProcesses = Set(processes)
        gateState = .listening
        gateLoggedWaiting = false
        probe.start(processes: processes, onFire: { [weak self] in
            guard let self, self.gate === probe else { return }
            self.gateState = .sustained
            self.stopGate()
            self.poll()
        }, completion: { [weak self] ok in
            guard let self, self.gate === probe, !ok else { return }
            Log.write("Media: sound check unavailable; starting on the open stream")
            self.gateState = .failed
            self.stopGate()
            self.poll()
        })
    }

    private func stopGate() {
        guard let probe = gate else { return }
        gate = nil
        gateProcesses = []
        if #available(macOS 14.2, *) { (probe as? PlaybackLevelProbe)?.stop() }
    }

    private func readSnapshot() -> Snapshot {
        var result = Snapshot()
        let rules = apps()
        let processes = Self.objects(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyProcessObjectList)
        if owners.count > processes.count + 32 { owners = owners.filter { processes.contains($0.key) } }
        for process in processes {
            let bundle = Self.string(process, kAudioProcessPropertyBundleID)
            if ignoredPrefixes.contains(where: { bundle.hasPrefix($0) }) { continue }
            let output = Self.flag(process, kAudioProcessPropertyIsRunningOutput)
            let input = Self.flag(process, kAudioProcessPropertyIsRunningInput)
            guard output || input else { continue }
            let recorder = recorderPrefixes.contains(where: { bundle.hasPrefix($0) })
            let owner = ownerBundle(process, bundle)
            noteSeen(owner)
            let decision = rules.decide(app: owner, output: output, input: input, recorder: recorder)
            if input { result.micApps.append(bundle) }
            if decision.playing {
                result.playingApps.append(bundle)
                result.playingProcesses.append(process)
            }
            if decision.call { result.callApps.append(bundle) }
            if decision.triggers {
                if !result.triggerOwners.contains(owner) { result.triggerOwners.append(owner) }
                if decision.playing { result.triggerProcesses.append(process) }
                if decision.callTrigger { result.callTriggers = true }
                if decision.trusted { result.trustedPlaying = true }
            }
        }
        if result.callApps.isEmpty { addBrowserMeetings(to: &result, rules: rules) }
        return result
    }

    /// Owner bundles per process object (a process keeps its object ID while it lives).
    private var owners: [AudioObjectID: String] = [:]

    private func ownerBundle(_ process: AudioObjectID, _ bundle: String) -> String {
        if let known = owners[process] { return known }
        var parent: String?
        if bundle.isEmpty || bundle == "?" {
            var address = AudioObjectPropertyAddress(mSelector: kAudioProcessPropertyPID, mScope: kAudioObjectPropertyScopeGlobal,
                                                     mElement: kAudioObjectPropertyElementMain)
            var pid: pid_t = 0
            var size = UInt32(MemoryLayout<pid_t>.size)
            if AudioObjectGetPropertyData(process, &address, 0, nil, &size, &pid) == noErr { parent = UserIntentWatcher.parentBundle(of: pid) }
        }
        let owner = UserIntent.owner(processBundle: bundle, parentBundle: parent)
        owners[process] = owner
        return owner
    }

    private func noteSeen(_ owner: String) {
        guard !owner.isEmpty, owner != "?" else { return }
        if seenApps.last == owner { return }
        seenApps.removeAll { $0 == owner }
        seenApps.append(owner)
        if seenApps.count > seenLimit { seenApps.removeFirst(seenApps.count - seenLimit) }
        UserDefaults.standard.set(seenApps, forKey: "seenAudioApps")
    }

    /// A browser that is playing audio while a meeting tab is open counts as a call (joined muted: no mic).
    /// A muted meeting tab (no mic) in a listed browser follows its mic settings: "keep" reports the call,
    /// "take on mic" takes the AirPods; a browser with none set is left out.
    private func addBrowserMeetings(to result: inout Snapshot, rules: TriggerAppList) {
        for browser in browsers where result.playingApps.contains(where: { $0.hasPrefix(browser.bundlePrefix) }) {
            let settings = rules.find(browser.bundlePrefix)?.settings
            if settings?.isNever == true { continue }
            refreshMeetingTabs(browser)
            guard meetingTabs[browser.appName] == true else { continue }
            if settings?.keepOnMic ?? true { result.callApps.append(browser.bundlePrefix + " (meeting tab)") }
            if settings?.takeOnMic ?? true {
                result.callTriggers = true
                if !result.triggerOwners.contains(browser.bundlePrefix) { result.triggerOwners.append(browser.bundlePrefix) }
            }
        }
    }

    private func refreshMeetingTabs(_ browser: (bundlePrefix: String, appName: String)) {
        let name = browser.appName
        if let last = lastTabCheck[name], Date().timeIntervalSince(last) < 5 { return }
        lastTabCheck[name] = Date()
        scriptQueue.async { [weak self] in
            guard let self else { return }
            let source = "tell application \"\(name)\" to get URL of every tab of every window"
            var error: NSDictionary?
            let output = NSAppleScript(source: source)?.executeAndReturnError(&error)
            var found = false
            if let output, output.numberOfItems > 0 {
                // A list of windows, each a list of URLs (or a flat list).
                var urls: [String] = []
                func collect(_ d: NSAppleEventDescriptor) {
                    if d.numberOfItems > 0 { for i in 1...d.numberOfItems { if let item = d.atIndex(i) { collect(item) } } }
                    else if let s = d.stringValue { urls.append(s) }
                }
                collect(output)
                found = urls.contains { self.meetingURL.firstMatch(in: $0, range: NSRange($0.startIndex..., in: $0)) != nil }
            } else if let error {
                Log.write("Meeting tabs (\(name)): \(error[NSAppleScript.errorMessage] ?? error)")
            }
            DispatchQueue.main.async {
                if self.meetingTabs[name] != found { Log.write("Meeting tab in \(name): \(found)") }
                self.meetingTabs[name] = found
            }
        }
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
