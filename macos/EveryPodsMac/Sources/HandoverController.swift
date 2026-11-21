// HandoverController.swift
//
// Decides who holds the AirPods. A port of the essentials of the Windows HandoverController
// (windows/src/HandoverController.cpp), using the 4-byte cross-device protocol:
//   * the phone asks for the AirPods (requestDisconnect / requestHandover): release them and reply
//   * the phone pings for status (requestConnectionStatus): reply, or it drops the link after ~1 min
//   * the user asks for them ("Take AirPods"): ask the phone to release, then connect here
// Everything runs on the main thread.

import Foundation

enum Ownership: Equatable {
    case unknown, thisMac, otherDevice
}

final class HandoverController {
    let peer = PeerLink()
    let pods = AirPodsConnector()
    let media = MediaWatcher()
    private(set) var ownership: Ownership = .unknown
    var onChange: (() -> Void)?

    private var pollTimer: Timer?
    private var takeContinuation: (() -> Void)?
    private var takeInProgress = false
    private var lastRelease = Date.distantPast
    private var announcedCall: Bool?
    /// After giving the AirPods away, do not auto-take them back for this long.
    private let autoTakeCooldown: TimeInterval = 10

    var phoneStatus: String {
        guard Settings.phoneAddress != nil else { return "Phone: not selected" }
        return peer.isConnected ? "Phone: connected" : "Phone: connecting…"
    }

    var airPodsStatus: String {
        guard pods.isConfigured else { return "AirPods: not selected" }
        switch ownership {
        case .thisMac: return "AirPods: connected to this Mac"
        case .otherDevice: return "AirPods: on another device"
        case .unknown: return "AirPods: not connected"
        }
    }

    var macAudioStatus: String {
        if media.snapshot.inCall { return "Mac audio: microphone in use" }
        if media.snapshot.playing { return "Mac audio: playing" }
        return "Mac audio: idle"
    }

    func start() {
        media.onPlaybackStarted = { [weak self] in self?.macStartedPlaying() }
        media.onChange = { [weak self] in
            self?.announceCallState()
            self?.onChange?()
        }
        media.start()
        peer.onPacket = { [weak self] data in self?.handle(data) }
        peer.onConnectionChange = { [weak self] connected in
            Log.write("Phone link \(connected ? "up" : "down")")
            if connected {
                self?.announce()
                self?.announcedCall = nil
                self?.announceCallState()
            }
            self?.onChange?()
        }
        restartPeer()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.refresh() }
        refresh()
    }

    func restartPeer() {
        if let address = Settings.phoneAddress { peer.start(address: address) } else { peer.stop() }
        onChange?()
    }

    /// After sleep the Bluetooth stack takes a moment to come back, and macOS may have reconnected
    /// the AirPods by itself. Rebuild the phone link from scratch, then re-announce who holds them.
    func handleWake() {
        Log.write("Wake: rebuilding the phone link")
        peer.stop()
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            self?.restartPeer()
            self?.refresh()
        }
    }

    // MARK: User actions

    /// Takes the AirPods for this Mac: ask the phone to release them, then connect.
    func takeAirPods() {
        guard pods.isConfigured, !takeInProgress else { return }
        guard !pods.isConnected else { return Log.write("Take: already connected here") }
        takeInProgress = true
        Log.write("Take: requesting AirPods")
        let connect = { [weak self] in
            guard let self else { return }
            self.pods.connect { success in
                self.takeInProgress = false
                Log.write("Take: connect \(success ? "succeeded" : "failed")")
                self.refresh()
            }
        }
        if peer.send(Packet.requestDisconnect) {
            // Proceed when the phone reports released, or after a short grace period.
            var done = false
            let proceed = { if !done { done = true; self.takeContinuation = nil; connect() } }
            takeContinuation = proceed
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5, execute: proceed)
        } else {
            Log.write("Take: phone link is down; connecting directly")
            connect()
        }
    }

    /// Gives the AirPods up (the phone or any other device can take them).
    func releaseAirPods() {
        guard pods.isConnected else { return }
        giveUpAirPods(reply: true)
    }

    /// Pauses what is playing on the AirPods (so it does not jump to the speakers), then disconnects.
    private func giveUpAirPods(reply: Bool) {
        lastRelease = Date()
        let wasPlaying = media.snapshot.playing
        if wasPlaying { MediaControl.togglePlayPause() }
        if media.snapshot.inCall { Log.write("Release: giving up the AirPods during a call (requester wins)") }
        // A short pause lets the player react before the audio route disappears.
        DispatchQueue.main.asyncAfter(deadline: .now() + (wasPlaying ? 0.4 : 0)) { [weak self] in
            guard let self else { return }
            self.pods.disconnect()
            self.ownership = .otherDevice
            if reply { self.peer.send(Packet.airPodsDisconnected) }
            self.onChange?()
        }
    }

    /// The Mac started playing audio: take the AirPods unless a cooldown or setting says no.
    private func macStartedPlaying() {
        guard Settings.autoTake, pods.isConfigured, peer.isConnected, !pods.isConnected, !takeInProgress else { return }
        guard Date().timeIntervalSince(lastRelease) > autoTakeCooldown else {
            return Log.write("Auto-take skipped (cooldown after releasing the AirPods)")
        }
        Log.write("Auto-take: this Mac started playing")
        takeAirPods()
    }

    /// Tells the phone whether the Mac is in a call; the Android app holds back takeovers during one.
    private func announceCallState() {
        let inCall = media.snapshot.inCall
        guard peer.isConnected, announcedCall != inCall else { return }
        announcedCall = inCall
        peer.send(inCall ? Packet.peerAudioActive : Packet.peerAudioIdle)
    }

    // MARK: Packets

    private func handle(_ data: Data) {
        switch Incoming(data) {
        case .requestConnectionStatus:
            announce()
        case .requestDisconnect, .requestHandover:
            Log.write("Phone requested the AirPods")
            if pods.isConnected {
                giveUpAirPods(reply: true)
            } else {
                peer.send(Packet.airPodsDisconnected)
                onChange?()
            }
        case .airPodsConnected:
            if !pods.isConnected { ownership = .otherDevice }
            onChange?()
        case .airPodsDisconnected:
            takeContinuation?()
            if ownership == .otherDevice { ownership = .unknown }
            onChange?()
        case .peerAudioActive, .peerAudioIdle, .other:
            break  // AirPods data frames and call/audio hints are not used yet
        }
    }

    /// Tells the phone whether this Mac currently holds the AirPods.
    private func announce() {
        peer.send(pods.isConnected ? Packet.airPodsConnected : Packet.airPodsDisconnected)
    }

    /// Polls the Bluetooth connection state so changes made outside the app are noticed.
    private func refresh() {
        let connected = pods.isConnected
        let previous = ownership
        if connected { ownership = .thisMac } else if ownership == .thisMac { ownership = .unknown }
        if ownership != previous {
            announce()
            onChange?()
        }
    }
}
