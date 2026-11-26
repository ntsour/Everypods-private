// PlaybackHold.swift
//
// Holds this Mac's audio while the AirPods connect, so nothing plays from the speakers and playback starts on
// the AirPods: the output that is playing is muted the moment playback is seen, the player is paused with the
// media key, and both are undone once the AirPods are the default output. For calls nothing is held; the
// same status only drives the progress panel. Mirrors the Android feature (docs/playback-hold.md there).
// Everything runs on the main thread.

import Foundation

enum HoldPhase: Equatable {
    case requestingRelease, connecting, routing, connected, failed
}

/// media: audio is held while connecting. call: progress only, nothing is held.
enum HoldKind: Equatable {
    case media, call
}

struct HoldStatus: Equatable {
    var phase: HoldPhase
    var kind: HoldKind
    var deviceName: String
    /// 1-based; incremented by `PlaybackHold.retry()`.
    var attempt: Int
    var phaseStartedAt: Date
    var attemptStartedAt: Date
    var attemptTimeout: TimeInterval

    /// Overall progress in 0...1 for a progress bar. Time-based within phase bands so it always moves:
    /// requestingRelease 0-0.3, connecting 0.3-0.7, routing 0.7-0.95, connected 1, failed 0.95.
    func progress(at now: Date = Date()) -> Double {
        let band: (Double, Double)
        switch phase {
        case .requestingRelease: band = (0, 0.3)
        case .connecting: band = (0.3, 0.7)
        case .routing: band = (0.7, 0.95)
        case .connected: return 1
        case .failed: return 0.95
        }
        let remaining = max(attemptStartedAt.addingTimeInterval(attemptTimeout).timeIntervalSince(phaseStartedAt), 0.001)
        let t = min(max(now.timeIntervalSince(phaseStartedAt) / remaining, 0), 1)
        let eased = 1 - (1 - t) * (1 - t)
        return band.0 + (band.1 - band.0) * eased
    }
}

/// The panel's view of the hold: it reads `status` (and `onStatusChange`) and calls the three actions.
protocol PlaybackHoldActions: AnyObject {
    var status: HoldStatus? { get }
    /// Called on the main thread whenever `status` changes (nil = no hold).
    var onStatusChange: ((HoldStatus?) -> Void)? { get set }
    /// failed -> try connecting again, keeping the hold.
    func retry()
    /// Give up on the AirPods: media resumes on this Mac's speakers; a call stays on the Mac.
    func playHere()
    /// failed -> end the hold, leave the media paused (a call: just close).
    func dismissStayPaused()
}

final class PlaybackHold: PlaybackHoldActions {
    // Timings, as on Android.
    private let attemptTimeout: TimeInterval = 10
    private let failedChoiceTimeout: TimeInterval = 12
    private let routePollInterval: TimeInterval = 0.15
    private let routePollMax: TimeInterval = 3
    private let connectedLinger: TimeInterval = 1.5
    /// A pre-mute (playback seen, takeover not decided yet) is lifted after this if no hold starts. It covers
    /// MediaWatcher's wait for real sound (tap setup plus 1 s of sound, about 1.5 s).
    private let preMuteMax: TimeInterval = 2.5
    /// Gap between the AirPods becoming the output and the resume key, so the player resumes on them.
    private let resumeDelay: TimeInterval = 0.3

    // Wired by HandoverController.
    var retryTakeover: ((HoldKind) -> Void)?
    var airPodsAreOutput: () -> Bool = { false }
    var airPodsAddress: () -> String? = { nil }
    var isPlaying: () -> Bool = { false }
    /// Bundle IDs of the processes playing audio now (MediaWatcher's snapshot).
    var playingApps: () -> [String] = { [] }

    var onStatusChange: ((HoldStatus?) -> Void)?
    private(set) var status: HoldStatus? {
        didSet { if status != oldValue { onStatusChange?(status) } }
    }

    /// True from `start` until released, failed-and-dismissed or cancelled (false while "connected" lingers).
    var isActive: Bool { guard let s = status else { return false }; return s.phase != .connected }
    var isMediaHold: Bool { isActive && status?.kind == .media }

    private var wePaused = false
    /// Who was playing when we pressed pause. The media key goes to macOS's Now Playing app, which can be a
    /// different (paused) app: then our key starts that app instead. `checkPause` and `release` use this set
    /// to spot that and never press play into the wrong app.
    private var pausedApps: Set<String> = []
    /// How long after the pause key `checkPause` looks for an app the key started by mistake.
    private let pauseCheckDelay: TimeInterval = 1
    private var timers: [String: DispatchWorkItem] = [:]
    private var routePollStarted = Date.distantPast

    init() {
        OutputMute.recoverIfNeeded()
    }

    // MARK: Hooks for HandoverController

    /// Playback was just seen and a takeover is likely: mute the speakers now, before the takeover is decided.
    func preMute() {
        guard !isActive, Settings.holdPlayback else { return }
        if OutputMute.muteDefaultOutput(unlessAddress: airPodsAddress()) {
            schedule("preMute", after: preMuteMax) { [weak self] in
                guard let self, !self.isActive else { return }
                OutputMute.restore("no takeover followed")
            }
        }
    }

    /// The takeover is starting (peers asked to release, or connecting directly).
    func start(kind: HoldKind, deviceName: String, peerAsked: Bool) {
        cancel("preMute")
        if isActive {
            if status?.kind == kind { return Log.write("Hold: already active, keeping it") }
            // A call during a media hold: the media stays paused, the call takes over the panel.
            endStayPaused("call takes over")
        }
        if kind == .media {
            guard Settings.holdPlayback else { return }
            OutputMute.muteDefaultOutput(unlessAddress: airPodsAddress())
            if isPlaying() {
                let before = Set(playingApps())
                if MediaControl.togglePlayPause() {
                    wePaused = true
                    pausedApps = before
                    schedule("pauseCheck", after: pauseCheckDelay) { [weak self] in self?.checkPause() }
                }
            }
        }
        let now = Date()
        status = HoldStatus(phase: peerAsked ? .requestingRelease : .connecting, kind: kind, deviceName: deviceName,
                            attempt: 1, phaseStartedAt: now, attemptStartedAt: now, attemptTimeout: attemptTimeout)
        Log.write("Hold: start \(kind) for \(deviceName) (paused=\(wePaused))")
        armAttemptTimeout()
    }

    /// The Bluetooth connect started (after the peer acknowledged, or its wait ran out).
    func onConnecting() {
        guard isActive, status?.phase == .requestingRelease else { return }
        setPhase(.connecting)
    }

    /// The AirPods connected: wait until they are the output, then release.
    func onConnected() {
        guard isActive, let s = status else { return }
        cancel("failedChoice")
        if s.phase == .failed { status?.attemptStartedAt = Date(); armAttemptTimeout() }
        setPhase(.routing)
        if s.kind == .call { return connected() }
        routePollStarted = Date()
        pollRoute()
    }

    /// All connect attempts failed: show the failure now instead of waiting for the attempt timeout.
    func onConnectFailed() {
        guard isActive, status?.phase != .failed else { return }
        fail("connect attempts failed")
    }

    /// The AirPods went to a peer, or the call ended: end quietly (media stays paused).
    func onLost(_ reason: String) {
        if isActive { endStayPaused(reason) }
    }

    func callEnded() {
        if status?.kind == .call { clear("call ended") }
    }

    // MARK: Panel actions

    func retry() {
        guard isActive, var s = status, s.phase == .failed else { return }
        cancel("failedChoice")
        let now = Date()
        s.attempt += 1
        s.phase = .connecting
        s.phaseStartedAt = now
        s.attemptStartedAt = now
        status = s
        Log.write("Hold: retry (attempt \(s.attempt))")
        armAttemptTimeout()
        retryTakeover?(s.kind)
    }

    func playHere() {
        guard status != nil else { return }
        Log.write("Hold: play here")
        release(resume: true)
        clear("play here")
    }

    func dismissStayPaused() { endStayPaused("dismissed") }

    // MARK: Internals

    private func setPhase(_ phase: HoldPhase) {
        guard var s = status, s.phase != phase else { return }
        s.phase = phase
        s.phaseStartedAt = Date()
        status = s
    }

    private func pollRoute() {
        guard isActive, status?.phase == .routing else { return }
        let elapsed = Date().timeIntervalSince(routePollStarted)
        if airPodsAreOutput() {
            Log.write("Hold: AirPods are the output after \(Int(elapsed * 1000)) ms")
            return connected()
        }
        if elapsed >= routePollMax {
            Log.write("Hold: AirPods not the output after \(Int(elapsed * 1000)) ms; releasing anyway")
            return connected()
        }
        schedule("routePoll", after: routePollInterval) { [weak self] in self?.pollRoute() }
    }

    private func connected() {
        cancelAll()
        setPhase(.connected)
        if status?.kind == .media { release(resume: true) }
        schedule("linger", after: connectedLinger) { [weak self] in
            guard let self, self.status?.phase == .connected else { return }
            self.status = nil
        }
    }

    private func fail(_ reason: String) {
        Log.write("Hold: failed (\(reason)); waiting for Retry / Play on Mac")
        cancel("attempt"); cancel("routePoll")
        setPhase(.failed)
        schedule("failedChoice", after: failedChoiceTimeout) { [weak self] in
            self?.endStayPaused("no choice within \(Int(self?.failedChoiceTimeout ?? 0)) s")
        }
    }

    private func armAttemptTimeout() {
        guard let s = status else { return }
        let left = max(s.attemptStartedAt.addingTimeInterval(s.attemptTimeout).timeIntervalSinceNow, 0.01)
        schedule("attempt", after: left) { [weak self] in
            guard let self, self.isActive, self.status?.phase != .failed else { return }
            self.fail("attempt \(self.status?.attempt ?? 0) timed out")
        }
    }

    /// Apps playing now that were not playing when we pressed pause: our key started them.
    private func startedByOurKey() -> [String] { playingApps().filter { !pausedApps.contains($0) } }

    /// After the pause key: if it started some other app (the Now Playing app was not the one playing), press
    /// again to stop that app and forget the pause, so the release never presses play into the wrong app.
    private func checkPause() {
        guard wePaused else { return }
        let started = startedByOurKey()
        let still = playingApps().filter { pausedApps.contains($0) }
        Log.write("Hold: pause check: started=\(started) stillPlaying=\(still)")
        if !started.isEmpty {
            Log.write("Hold: pause key went to another app; undoing it and not resuming later")
            MediaControl.togglePlayPause()
            wePaused = false
        }
    }

    /// Unmute, and press play again if we paused (resume = false keeps it paused).
    private func release(resume: Bool) {
        cancelAll()
        OutputMute.restore(resume ? "released" : "stay paused")
        if resume, wePaused {
            let pausedApps = self.pausedApps
            DispatchQueue.main.asyncAfter(deadline: .now() + resumeDelay) { [weak self] in
                guard let self else { return }
                // An app that wasn't playing at the pause is playing now: the key would stop it, not resume ours.
                // The paused app itself can still look "playing": browsers keep their audio stream open for a
                // while after a pause, so that says nothing about whether the pause worked.
                let started = self.playingApps().filter { !pausedApps.contains($0) }
                if !started.isEmpty {
                    return Log.write("Hold: not resuming, \(started) started meanwhile (paused \(pausedApps.sorted()))")
                }
                Log.write("Hold: resuming \(pausedApps.sorted())")
                MediaControl.togglePlayPause()
            }
        }
        wePaused = false
        pausedApps = []
    }

    private func endStayPaused(_ reason: String) {
        guard status != nil else { return }
        Log.write("Hold: end, media left paused (\(reason))")
        release(resume: false)
        clear(reason)
    }

    private func clear(_ reason: String) {
        cancelAll()
        status = nil
    }

    private func schedule(_ key: String, after delay: TimeInterval, _ block: @escaping () -> Void) {
        timers[key]?.cancel()
        let item = DispatchWorkItem(block: block)
        timers[key] = item
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: item)
    }

    private func cancel(_ key: String) { timers.removeValue(forKey: key)?.cancel() }

    private func cancelAll() {
        for key in ["attempt", "failedChoice", "routePoll", "linger", "preMute", "pauseCheck"] { cancel(key) }
    }
}
