// HandoverController.swift
//
// Decides who holds the AirPods. A rule-for-rule port of windows/src/HandoverController.cpp: same ownership
// states, same guard timers (debounce, anti-ping-pong, voluntary-release suppression), same packet handling.
// Holder rules follow the peer spec: pods in the case -> ignore; in a call -> keep them; right after our own
// takeover -> keep them (media only); otherwise release. Differences are listed in PLAN.md. Everything runs on
// the main thread.

import Foundation

enum Ownership: Equatable {
    case unknown, thisMac, otherDevice
}

/// Why this Mac takes the AirPods: media sends REQUEST_DISCONNECT, a call sends REQUEST_HANDOVER.
enum TakeIntent {
    case media, call
}

final class HandoverController {
    let peers = PeerRegistry()
    let pods = AirPodsConnector()
    let media = MediaWatcher()
    let router = AudioRouter()
    let proximity = ProximityWatcher()
    private(set) var ownership: Ownership = .unknown
    var onChange: (() -> Void)?

    // Timers, as in the Windows client.
    private let debounce: TimeInterval = 0.3
    private let antiPingPong: TimeInterval = 3
    private let voluntarySuppress: TimeInterval = 30
    private let postReleaseWindow: TimeInterval = 5
    private let ackWait: TimeInterval = 1.5
    private let releaseGrace: TimeInterval = 4.5

    private var lastAction = Date.distantPast
    private var lastLocalTakeover = Date.distantPast
    private var lastLostOwnership = Date.distantPast
    private var lastVoluntaryRelease = Date.distantPast
    private var voluntarilyReleased = false
    private var voluntaryReleaseApp = ""
    private var takeoverInFlight = false
    private var releasing = false
    private var lastBluetoothConnected = false
    private var ackWaiters: [() -> Void] = []
    private var announcedCall: Bool?
    private var callDebounce: DispatchWorkItem?
    private var pollTimer: Timer?
    /// An automatic takeover trigger held back by a live acquisition reservation; replayed when it ends.
    private var triggerDeferredByReservation = false

    /// Protocol v2: this Mac's snapshots and its vote (grantor role) in a phone's lid-open acquisition round.
    lazy var v2: PeerV2Hub = {
        let hub = PeerV2Hub(installationId: Settings.installationId)
        hub.localState = { [weak self] in self?.v2LocalState() ?? PeerV2Hub.LocalState(headsetId: nil, ownership: .unknown, audioUse: .unknown, takeoverActive: false) }
        hub.sendRecord = { [weak self] id, payload in self?.peers.sendV2(payload, to: id) ?? false }
        hub.isConfigured = { [weak self] id in self?.peers.isLinked(id) ?? false }
        hub.configuredPeerIds = { [weak self] in self?.peers.configuredInstallationIds() }
        hub.peerName = { [weak self] id in self?.peers.displayName(forInstallationId: id) ?? String(id.prefix(8)) }
        hub.onReservationEnded = { [weak self] in self?.reservationEnded() }
        return hub
    }()

    private func v2LocalState() -> PeerV2Hub.LocalState {
        var headsetId: String?
        if let address = Settings.airPodsAddress { headsetId = try? PeerV2Message.headsetId(fromAddress: address) }
        var held: PeerOwnership = .unknown
        if headsetId != nil {
            if ownership == .thisMac || pods.isConnected { held = .held } else if ownership == .otherDevice { held = .released }
        }
        let use: PeerAudioUse = media.snapshot.inCall ? .call : media.snapshot.playing ? .media : .idle
        return PeerV2Hub.LocalState(headsetId: headsetId, ownership: held, audioUse: use, takeoverActive: takeoverInFlight)
    }

    /// Replays the automatic trigger that a reservation held back, if the audio that caused it is still going.
    private func reservationEnded() {
        guard triggerDeferredByReservation, !v2.reservationLive else { return }
        triggerDeferredByReservation = false
        guard media.snapshot.playing || media.snapshot.inCall else {
            return Log.write("V2: reservation ended; the audio that triggered a takeover has stopped")
        }
        Log.write("V2: reservation ended; running the deferred takeover trigger")
        onMediaPlayingChanged(true)
    }

    // MARK: Status text (menu and tooltip)

    var airPodsStatus: String {
        guard pods.isConfigured else { return "AirPods: not selected" }
        switch ownership {
        case .thisMac: return "AirPods: on this Mac"
        case .otherDevice: return "AirPods: on Android"
        case .unknown: return "AirPods: unknown"
        }
    }

    var macAudioStatus: String {
        if media.snapshot.inCall { return "Mac audio: in a call" }
        if media.snapshot.micInUse { return "Mac audio: microphone in use" }
        if media.snapshot.playing { return "Mac audio: playing" }
        return "Mac audio: idle"
    }

    // MARK: Lifecycle

    func start() {
        media.onPlaybackStarted = { [weak self] in self?.onMediaPlayingChanged(true) }
        media.onPlaybackStopped = { [weak self] in self?.onMediaPlayingChanged(false) }
        media.onChange = { [weak self] in
            self?.scheduleCallState()
            self?.v2.stateChanged()
            self?.onChange?()
        }
        peers.onPacket = { [weak self] data in self?.onIncomingPacket(data) }
        peers.onConnectionChange = { [weak self] address, connected in self?.onPeerConnectionChanged(address, connected) }
        peers.onV2LinkChange = { [weak self] id, up in
            if up { self?.v2.linkUp(id) } else { self?.v2.linkDown(id) }
        }
        peers.onV2Record = { [weak self] id, payload in self?.v2.onRecord(id, payload) }
        v2.start()
        media.start()
        proximity.onLidOpened = { [weak self] in self?.onLidOpened() }
        proximity.start()
        peers.start()
        lastBluetoothConnected = pods.isConnected
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.pollBluetooth() }
        pollBluetooth()
    }

    /// After sleep the Bluetooth stack takes a moment to come back, and macOS may have reconnected the AirPods
    /// by itself. Rebuild the phone links from scratch, then re-resolve who holds the AirPods.
    /// Before system sleep: close the phone links and stop every Bluetooth scan. Left running, the scans and the
    /// pending reconnects keep waking the Mac (DarkWake "bluetooth") up to ~190 times an hour.
    func handleSleep() {
        Log.write("Sleep: closing the phone links and stopping Bluetooth scans")
        asleep = true
        peers.stop()
        proximity.stop()
    }

    func handleWake() {
        Log.write("Wake: rebuilding the phone links")
        asleep = false
        peers.stop()
        proximity.start()
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self, !self.asleep else { return }
            self.peers.start()
            self.pollBluetooth()
        }
    }

    /// True between a sleep notification and the next full wake (dark wakes don't count).
    private(set) var asleep = false

    // MARK: Ownership

    /// Returns true if the state changed, so callers announce only on real transitions.
    @discardableResult
    private func setState(_ state: Ownership) -> Bool {
        guard state != ownership else { return false }
        ownership = state
        Log.write("State -> \(state)")
        v2.stateChanged()
        router.setPersistent(state == .thisMac)
        if state == .thisMac {
            updateLidLease(true, "the AirPods are on this Mac")
            // Claiming the AirPods by any means resets the voluntary-release gate.
            voluntarilyReleased = false
            voluntaryReleaseApp = ""
        }
        onChange?()
        return true
    }

    /// Tells every connected phone whether this Mac holds the AirPods.
    private func sendState() {
        // While releasing, the phone is waiting for AIRPODS_DISCONNECTED; a "connected" now would read as a refusal.
        if releasing { return }
        peers.send(ownership == .thisMac ? Packet.airPodsConnected : Packet.airPodsDisconnected)
    }

    private func since(_ date: Date) -> TimeInterval { Date().timeIntervalSince(date) }

    /// Bluetooth edge observer (1 s): catches connects and disconnects that bypass the protocol (Apple
    /// auto-switch, a manual connect, the phone grabbing the AirPods before the link was up).
    private func pollBluetooth() {
        let connected = pods.isConnected

        if ownership == .unknown, peers.isAnyConnected || connected {
            if setState(connected ? .thisMac : .otherDevice) { sendState() }
        }

        if connected && !lastBluetoothConnected {
            if setState(.thisMac) {
                Log.write("Bluetooth: AirPods connected here (rising edge); claiming ownership")
                sendState()
                if Settings.autoRoute { router.routeToAirPods() }
            }
        } else if !connected && lastBluetoothConnected {
            if ownership == .thisMac {
                Log.write("Bluetooth: AirPods left this Mac (falling edge); releasing")
                lastLostOwnership = Date()
                if setState(.otherDevice) { sendState() }
            }
        }
        lastBluetoothConnected = connected
    }

    // MARK: Lid-open connect

    /// The phones' "Auto-connect on case open (last device)": a lid opening is left to the device that used the
    /// AirPods last. The lease says that was this Mac: set when the AirPods come here and kept while they sit in
    /// the case; cleared when a phone claims them or this Mac releases them.
    private func updateLidLease(_ held: Bool, _ why: String) {
        guard Settings.lidLease != held else { return }
        Settings.lidLease = held
        Log.write("LID: this Mac is \(held ? "" : "no longer ")the last device to use the AirPods (\(why))")
    }

    /// A case lid opened near this Mac: connect the AirPods here if this Mac used them last.
    private func onLidOpened() {
        guard Settings.lidOpenConnect, pods.isConfigured else { return }
        guard Settings.lidLease else {
            return Log.write("LID: case opened; another device used the AirPods last, leaving it to that one")
        }
        if ownership == .thisMac || pods.isConnected { return }
        if v2.anyPeerHoldsAirPods { return updateLidLease(false, "a phone holds the AirPods") }
        if v2.reservationLive { return Log.write("LID: case opened during a phone's acquisition; not connecting") }
        guard !takeoverInFlight else { return }
        takeoverInFlight = true
        // The phones wait 400 ms after the lid opens before connecting.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self else { return }
            guard !self.pods.isConnected, !self.v2.anyPeerHoldsAirPods else {
                self.takeoverInFlight = false
                return
            }
            Log.write("LID: case opened and this Mac used the AirPods last: connecting them")
            if Settings.autoRoute { self.router.setPersistent(true) }
            self.pods.connect { success in
                self.takeoverInFlight = false
                if success {
                    if self.setState(.thisMac) { self.sendState() }
                    if Settings.autoRoute { self.router.routeToAirPods() }
                    Log.write("LID: connected")
                } else {
                    self.router.setPersistent(self.ownership == .thisMac)
                    Log.write("LID: lid-open connect failed")
                }
            }
        }
    }

    // MARK: Media and call triggers

    /// Port of onMediaPlayingChanged: audio started (or stopped) on this Mac.
    func onMediaPlayingChanged(_ playing: Bool) {
        if !playing {
            // Only clear the voluntary-release gate once past the immediate post-release window: a stop
            // inside it is our own pause during the handover.
            if since(lastVoluntaryRelease) > postReleaseWindow, voluntarilyReleased {
                voluntarilyReleased = false
                // Playback really stopped, so the next start is the user pressing play: the same app may take
                // the AirPods again (the app window only guards against a player that never stopped).
                voluntaryReleaseApp = ""
                Log.write("Gate: voluntary release cleared (media stopped)")
            }
            return
        }
        let intent: TakeIntent = media.snapshot.inCall ? .call : .media
        guard pods.isConfigured else { return }
        guard intent == .call ? Settings.takeOnCall : Settings.takeOnMediaStart else {
            return Log.write("Skip: take over on \(intent == .call ? "call" : "media start") is off")
        }
        if since(lastAction) < debounce { return }
        lastAction = Date()

        if pods.isConnected {
            if ownership == .thisMac { return }
            // Already connected at the Bluetooth layer but the state says otherwise: claim without
            // reconnecting (reconnecting would tear down the live link).
            Log.write("Claim: AirPods already connected here; claiming ownership")
            setState(.thisMac)
            lastLocalTakeover = Date()
            sendState()
            if Settings.autoRoute { router.routeToAirPods() }
            return
        }

        // A phone's lid-open acquisition holds a grant from this Mac: do not take the AirPods out from under it.
        // The trigger is replayed when the reservation ends (the normal takeover then asks the new holder to release).
        if v2.reservationLive {
            triggerDeferredByReservation = true
            lastAction = Date.distantPast
            return Log.write("Skip: a phone's acquisition holds a reservation on this Mac; deferring the takeover")
        }

        // Not connected here. Do not poke Bluetooth when the AirPods are in their case.
        if !proximity.airPodsAvailable {
            return Log.write("Skip: AirPods appear to be in the case; not initiating a takeover")
        }
        if since(lastLostOwnership) < antiPingPong {
            return Log.write("Skip: anti-ping-pong (lost the AirPods \(Int(since(lastLostOwnership) * 1000)) ms ago)")
        }
        if !voluntaryReleaseApp.isEmpty, since(lastVoluntaryRelease) < voluntarySuppress,
           media.currentAppID == voluntaryReleaseApp {
            return Log.write("Skip: \(voluntaryReleaseApp) re-triggered within the voluntary-release window")
        }
        guard !takeoverInFlight else { return Log.write("Skip: a takeover is already in flight") }
        Log.write("Trigger: \(intent == .call ? "call" : "audio") active (\(media.currentAppID.isEmpty ? "unknown app" : media.currentAppID)); taking the AirPods")
        startTakeover(intent)
    }

    /// A call (or meeting) started on this Mac: an explicit user action, so the gate is cleared first.
    private func onCallActive() {
        if voluntarilyReleased {
            voluntarilyReleased = false
            Log.write("Gate: voluntary release cleared (call active)")
        }
        onMediaPlayingChanged(true)
    }

    private func onCallEnded() {
        if voluntarilyReleased {
            voluntarilyReleased = false
            Log.write("Gate: voluntary release cleared (call ended)")
        }
    }

    // MARK: User actions

    /// "Connect AirPods now": bypasses the proximity probe and the guards.
    func connectNow() {
        guard pods.isConfigured else { return }
        if pods.isConnected {
            if setState(.thisMac) { sendState() }
            router.routeToAirPods()
            return
        }
        guard !takeoverInFlight else { return }
        // The user chose to connect here: the reservation yields instead of holding the click back.
        v2.cancelReservationForUserTakeover()
        Log.write("Manual connect")
        startTakeover(.media)
    }

    /// Gives the AirPods up on request from the menu (any phone can then take them).
    func releaseAirPods() {
        guard pods.isConnected else { return }
        lastLostOwnership = Date()
        giveUp()
    }

    // MARK: Taking and giving up

    /// Asks the peers to release, then connects (3 attempts). The connect starts when a phone confirms the
    /// release or after a short wait, whichever comes first.
    private func startTakeover(_ intent: TakeIntent) {
        takeoverInFlight = true
        // Select the AirPods as output the moment their audio device appears, rather than after the connect
        // completes, so audio does not start on the speakers.
        if Settings.autoRoute { router.setPersistent(true) }
        let connect = { [weak self] in
            guard let self else { return }
            self.pods.connectWithRetries { success in
                self.takeoverInFlight = false
                if success {
                    let claimed = self.setState(.thisMac)
                    self.lastLocalTakeover = Date()
                    if claimed { self.sendState() }
                    if Settings.autoRoute { self.router.routeToAirPods() }
                    Log.write("Take: connected")
                    if Settings.autoRoute { self.verifyAudioClaimed() }
                } else {
                    // Anti-ping-pong so a re-trigger does not retry immediately.
                    self.lastLostOwnership = Date()
                    self.router.setPersistent(self.ownership == .thisMac)
                    Log.write("Take: failed after 3 attempts; AirPods not acquired")
                }
            }
        }
        if peers.send(intent == .call ? Packet.requestHandover : Packet.requestDisconnect) {
            var started = false
            let go = { if !started { started = true; connect() } }
            ackWaiters.append(go)
            DispatchQueue.main.asyncAfter(deadline: .now() + ackWait, execute: go)
        } else {
            connect()
        }
    }

    /// Logs whether the AirPods became the audio output shortly after connecting. When macOS keeps audio on the
    /// speakers after a successful connect, check the Mac's pairing first: on 2026-11-22 a broken pairing record
    /// made macOS drop the AirPods right after every connect, and re-pairing them in System Settings fixed it.
    private func verifyAudioClaimed() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self, self.ownership == .thisMac, self.pods.isConnected else { return }
            Log.write(self.router.airPodsAreDefaultOutput ? "Take: AirPods are the audio output"
                                                         : "Take: AirPods connected but not the audio output")
        }
    }

    /// Optionally pauses what is playing (so audio does not jump to the speakers), then asks the AirPods to
    /// disconnect, lets the link close on its own and forces it only if it is still up after 4.5 s. The peers
    /// are told once it is down (the phone's acknowledgement timeout is 6 s).
    private func giveUp() {
        updateLidLease(false, "released the AirPods")
        let pause = Settings.pauseOnHandover && media.snapshot.playing
        // Set before the pause wait too: a status ping answered "connected" in that window reads as a refusal.
        releasing = true
        if pause { MediaControl.togglePlayPause() }
        // A short wait lets the player react before the audio route disappears.
        DispatchQueue.main.asyncAfter(deadline: .now() + (pause ? 0.4 : 0)) { [weak self] in
            guard let self else { return }
            self.pods.disconnect()
            let deadline = Date().addingTimeInterval(self.releaseGrace)
            func finish() {
                self.releasing = false
                self.setState(.otherDevice)
                self.lastBluetoothConnected = false
                self.peers.send(Packet.airPodsDisconnected)
            }
            func wait() {
                if !self.pods.isConnected { return finish() }
                if Date() >= deadline {
                    Log.write("Release: link still up after \(self.releaseGrace)s; forcing it")
                    self.pods.disconnect()
                    return finish()
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: wait)
            }
            wait()
        }
    }

    /// A phone asked for the AirPods and we agree: remember it so the same app cannot take them straight back.
    private func releaseToPeer() {
        voluntarilyReleased = true
        voluntaryReleaseApp = media.currentAppID
        lastVoluntaryRelease = Date()
        lastLostOwnership = Date()
        giveUp()
    }

    // MARK: Packets

    func onIncomingPacket(_ data: Data) {
        let holder = ownership == .thisMac || pods.isConnected
        let packet = Incoming(data)
        switch packet {
        case .requestDisconnect, .requestHandover:
            let call = packet == .requestHandover
            let label = call ? "requestHandover" : "requestDisconnect"
            // Only the holder can release; a non-holder just says it does not hold them.
            guard holder else {
                Log.write("In: \(label) ignored (not the holder)")
                peers.send(Packet.airPodsDisconnected)
                return
            }
            // Pods in the case: nothing to hand over.
            if !proximity.airPodsAvailable {
                return Log.write("In: \(label) ignored (AirPods appear to be in the case)")
            }
            // In a call or ringing: keep the AirPods, both request types.
            if media.snapshot.inCall {
                Log.write("In: \(label) refused (call active on this Mac)")
                peers.send(Packet.airPodsConnected)
                return
            }
            // Ping-pong guard: a media request right after our own takeover.
            if !call, since(lastLocalTakeover) < antiPingPong {
                Log.write("In: \(label) rejected (anti-ping-pong)")
                peers.send(Packet.airPodsConnected)
                return
            }
            Log.write("In: \(label) accepted; releasing the AirPods")
            releaseToPeer()

        case .airPodsConnected:
            // The AirPods can be connected to one device only. If this Mac's own Bluetooth link is up, the
            // phone's claim is stale (it announces its last known state when its link comes up): correct it.
            if pods.isConnected {
                Log.write("In: a phone claims the AirPods, but they are connected here; correcting it")
                setState(.thisMac)
                sendState()
                return
            }
            Log.write("In: a phone now holds the AirPods")
            updateLidLease(false, "a phone claimed the AirPods")
            lastLostOwnership = Date()
            setState(.otherDevice)

        case .airPodsDisconnected:
            // Wake a takeover that is waiting for the phone to confirm it released.
            let waiters = ackWaiters
            ackWaiters = []
            waiters.forEach { $0() }

        case .requestConnectionStatus:
            sendState()

        case .peerAudioActive, .peerAudioIdle, .other:
            break  // not used by this client
        }
    }

    func onPeerConnectionChanged(_ address: String, _ connected: Bool) {
        Log.write("Peer \(address) \(connected ? "connected" : "disconnected")")
        if connected {
            let here = pods.isConnected
            let wasLocal = ownership == .thisMac
            // Send our state only if it changed; the other phones already know.
            if setState(here ? .thisMac : .otherDevice) { sendState() }
            // We held the AirPods but they are gone: a phone took them over Bluetooth before this link was up.
            if wasLocal && !here {
                Log.write("Reclaim: a phone grabbed the AirPods before the link was up")
                onMediaPlayingChanged(true)
            }
            // A phone that connects late learns the current call state.
            if let call = announcedCall { peers.send(call ? Packet.peerAudioActive : Packet.peerAudioIdle) }
        }
        onChange?()
    }

    // MARK: Call state (reported to the phones; Mac addition)

    /// Debounced: a call is reported after 2 s, and ends only after 10 s of quiet, because browser calls
    /// toggle their audio flags (measured 5 s on / 5 s off blips) and a false "idle" would let a phone take
    /// the AirPods out from under a live call.
    private func scheduleCallState() {
        callDebounce?.cancel()
        let inCall = media.snapshot.inCall
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.announcedCall != inCall else { return }
            let first = self.announcedCall == nil
            self.announcedCall = inCall
            if inCall {
                Log.write("Call started (\(self.media.snapshot.callApps.joined(separator: ", ")))")
                self.onCallActive()
            } else if !first {
                Log.write("Call ended")
                self.onCallEnded()
            }
            self.peers.send(inCall ? Packet.peerAudioActive : Packet.peerAudioIdle)
        }
        callDebounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + (inCall ? 2 : 10), execute: work)
    }
}
