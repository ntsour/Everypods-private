// PeerV2Hub.swift
//
// Runs the v2 protocol over every ready phone link and votes in passive acquisition (the "acquire-v1" grantor
// role, never the requester): one PeerV2Session per link, this Mac's snapshot, the participant reservation and the
// 5 s grant expiry. The Mac is the LE central, so it initiates v2 (HELLO, then SNAPSHOT/PING after the peer's first
// SNAPSHOT) and restarts the handshake 60 s after a failure while the LE link stays up (PeerLeV2Links.kt).
//
// While a reservation is live HandoverController must not start an automatic takeover; it asks `reservationLive`
// and is told when the reservation ends. Everything runs on the main thread.

import Foundation

final class PeerV2Hub {
    /// What the controller measures; the hub turns it into snapshots and decisions.
    struct LocalState: Equatable {
        var headsetId: String?
        var ownership: PeerOwnership
        var audioUse: PeerAudioUse
        /// This computer is in the middle of taking the AirPods (not yet holding them): it denies requests as HELD.
        var takeoverActive: Bool
    }

    static let retryDelayMs: Int64 = 60_000

    let installationId: String
    let sessionId: String
    let coordinator: ParticipantCoordinator

    var localState: () -> LocalState = { LocalState(headsetId: nil, ownership: .unknown, audioUse: .unknown, takeoverActive: false) }
    /// Sends a v2 record payload to the phone with this installation ID.
    var sendRecord: (String, Data) -> Bool = { _, _ in false }
    /// True while the phone is a configured peer with a live link.
    var isConfigured: (String) -> Bool = { _ in false }
    /// Installation IDs of every configured peer (phones and computers), or nil when one of them is not known yet.
    var configuredPeerIds: () -> Set<String>? = { nil }
    var peerName: (String) -> String = { String($0.prefix(8)) }
    /// A reservation ended (resolved, cancelled or expired); called asynchronously on the main queue.
    var onReservationEnded: (() -> Void)?

    private let clock: () -> Int64
    private let log: (String) -> Void
    private let revisions = PeerSnapshotRevision()
    private var protocolHandler: PeerAcquisitionProtocol!
    private var links: [String: Link] = [:]
    /// Peers whose latest snapshot says they hold our AirPods. A phone measures this from its own Bluetooth
    /// profile, so it drops when the AirPods leave it (e.g. into the case), unlike the legacy AIRPODS_* packets,
    /// which a phone only sends around a handover.
    private var peersHolding = Set<String>()
    var anyPeerHoldsAirPods: Bool { !peersHolding.isEmpty }
    private var timer: Timer?
    private var lastBroadcast: [String: JSONValue]?
    /// After the requester's link dropped mid-acquisition the outcome is unknown: report ownership "unknown" until a
    /// fresh snapshot from that requester arrives or this computer's own measured state changes.
    private var unknownAfterDrop: (requesterId: String, measured: PeerOwnership)?

    private final class Link: PeerV2SessionHooks {
        let peerId: String
        unowned let hub: PeerV2Hub
        var session: PeerV2Session?
        var retryAtMs: Int64?
        init(peerId: String, hub: PeerV2Hub) { self.peerId = peerId; self.hub = hub }

        func send(_ frame: PeerV2Frame.Frame) -> Bool {
            guard let bytes = try? frame.encode() else { return false }
            return hub.sendRecord(peerId, bytes)
        }
        func isAuthorized() -> Bool { hub.isConfigured(peerId) }
        func localInstallationId() -> String { hub.installationId }
        func localSessionId() -> String { hub.sessionId }
        func localSnapshot() -> [String: JSONValue] { hub.localSnapshot() }
        func newConnectionId() -> String { UUID().uuidString.lowercased() }
        func bindPeerIdentity(_ installationId: String) throws {
            // The link was matched to a configured peer by the LE HELLO; the v2 HELLO must claim the same identity.
            guard installationId == peerId else { throw V2Error("v2_identity_mismatch") }
        }
        private var loggedReadySession: String?

        func observeSnapshot(remoteSessionId: String, snapshot: [String: JSONValue]) -> Bool {
            hub.observe(peerId, snapshot)
        }
        func markReady(remoteSessionId: String, acquisitionCapable: Bool) {
            // Called on every acknowledged heartbeat: log only the first time per peer session.
            guard loggedReadySession != remoteSessionId else { return }
            loggedReadySession = remoteSessionId
            hub.log("V2[\(hub.peerName(peerId))]: ready\(acquisitionCapable ? "" : " (peer lacks acquire-v1)") (snapshot rev \(hub.currentRevision()), ownership \(hub.measuredOwnershipText()))")
        }
        func handleAcquisition(remoteSessionId: String, message: PeerV2Message.Validated) throws {
            try hub.handleAcquisition(self, remoteSessionId, message)
        }
        func onEnded(reason: String) {
            hub.log("V2[\(hub.peerName(peerId))]: session ended (\(reason)); retrying in \(PeerV2Hub.retryDelayMs / 1000) s")
            session = nil
            retryAtMs = hub.clock() + PeerV2Hub.retryDelayMs
            hub.peerGone(peerId)
        }
    }

    init(installationId: String, sessionId: String = UUID().uuidString.lowercased(),
         clock: @escaping () -> Int64 = PeerV2Hub.monotonicMs, log: @escaping (String) -> Void = { Log.write($0) }) {
        self.installationId = installationId.lowercased()
        self.sessionId = sessionId
        self.clock = clock
        self.log = log
        coordinator = ParticipantCoordinator(localInstallationId: installationId, clock: clock)
        protocolHandler = PeerAcquisitionProtocol(localInstallationId: self.installationId,
                                                  localSessionId: { [sessionId] in sessionId }, coordinator: coordinator)
        coordinator.onTransition = { [weak self] requestId, phase, reason in self?.reservationChanged(requestId, phase, reason) }
    }

    static func monotonicMs() -> Int64 { Int64(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1_000_000) }

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        for id in Array(links.keys) { linkDown(id) }
    }

    // MARK: Links

    /// The LE link to a configured phone is ready: start (or restart) its v2 session.
    func linkUp(_ peerId: String) {
        links[peerId]?.session?.end()
        let link = Link(peerId: peerId, hub: self)
        links[peerId] = link
        begin(link)
    }

    /// The LE link ended: its v2 state is gone with it.
    func linkDown(_ peerId: String) {
        guard let link = links.removeValue(forKey: peerId) else { return }
        setPeerHolding(peerId, false)
        link.session?.end()
        link.session = nil
        peerGone(peerId)
    }

    func onRecord(_ peerId: String, _ payload: Data) { links[peerId]?.session?.onRecord(payload) }

    private func begin(_ link: Link) {
        link.retryAtMs = nil
        let session = PeerV2Session(hooks: link, clock: clock)
        link.session = session
        session.start()
    }

    private func tick() {
        let now = clock()
        for link in Array(links.values) {
            if let session = link.session {
                session.tick()
            } else if let retry = link.retryAtMs, now >= retry, links[link.peerId] === link {
                log("V2[\(peerName(link.peerId))]: starting the handshake again")
                begin(link)
            }
        }
        coordinator.expire()
        flush()
    }

    /// The peer's v2 state is gone (session ended or link down). A requester that vanished while its acquisition
    /// was in progress may or may not have connected the AirPods: free the reservation, and report ownership
    /// unknown until it is heard from again.
    private func peerGone(_ peerId: String) {
        if let r = coordinator.reservation, r.phase == .inProgress, r.requesterId == peerId {
            unknownAfterDrop = (peerId, measuredOwnership())
            coordinator.participantResolve(requestId: r.requestId, reason: "requester_link_dropped")
        }
        flush()
    }

    // MARK: Local state

    private func measuredOwnership() -> PeerOwnership { localState().ownership }

    func measuredOwnershipText() -> String { snapshotOwnership(localState()) }

    private func snapshotOwnership(_ state: LocalState) -> String {
        if unknownAfterDrop != nil { return "unknown" }
        switch state.ownership {
        case .held: return "held"
        case .released: return "released"
        case .unknown: return "unknown"
        }
    }

    private func audioUseText(_ use: PeerAudioUse) -> String {
        switch use {
        case .idle: return "idle"
        case .media: return "media"
        case .call: return "call"
        case .unknown: return "unknown"
        }
    }

    func localSnapshot() -> [String: JSONValue] {
        let state = localState()
        // A measured change of our own state ends the "unknown after the requester dropped" period.
        if let pending = unknownAfterDrop, pending.measured != state.ownership { unknownAfterDrop = nil }
        var snapshot: [String: JSONValue] = [
            "headsetId": state.headsetId.map { .string($0) } ?? .null,
            "ownership": .string(snapshotOwnership(state)),
            "audioUse": .string(audioUseText(state.audioUse)),
            "lastOwnerId": .null,
            "lastOwnerRevision": .null,
        ]
        let revision = revisions.forState(snapshot)
        snapshot["acquisition"] = acquisitionSnapshot()
        snapshot["sessionId"] = .string(sessionId)
        snapshot["revision"] = .int(revision)
        return snapshot
    }

    private func acquisitionSnapshot() -> JSONValue {
        guard let r = coordinator.live else { return .null }
        return .object(["requestId": .string(r.requestId), "requesterId": .string(r.requesterId),
                        "phase": .string(r.phase == .inProgress ? "in_progress" : "preparing")])
    }

    func currentRevision() -> Int64 { localSnapshot()["revision"]?.intValue ?? 0 }

    private func localStatus() -> LocalStatus {
        let state = localState()
        let snapshot = localSnapshot()
        var ownership: PeerOwnership = state.ownership
        if unknownAfterDrop != nil { ownership = .unknown }
        if state.takeoverActive { ownership = .held }
        return LocalStatus(installationId: installationId, sessionId: sessionId, revision: snapshot["revision"]?.intValue ?? 0,
                           ready: true, headsetId: state.headsetId, ownership: ownership, audioUse: state.audioUse)
    }

    /// The controller's state changed (ownership, audio): tell every ready peer if the snapshot differs.
    func stateChanged() { flush() }

    private func flush() {
        let snapshot = localSnapshot()
        guard snapshot != lastBroadcast else { return }
        lastBroadcast = snapshot
        guard let frame = try? PeerV2Message.encode(.snapshot, snapshot) else { return }
        for link in links.values { link.session?.sendFrame(frame) }
    }

    // MARK: Peer snapshots

    private func setPeerHolding(_ peerId: String, _ held: Bool) {
        let changed = held ? peersHolding.insert(peerId).inserted : peersHolding.remove(peerId) != nil
        if changed { log("V2[\(peerName(peerId))]: \(held ? "holds" : "no longer holds") the AirPods") }
    }

    private func observe(_ peerId: String, _ snapshot: [String: JSONValue]) -> Bool {
        let ours = localState().headsetId
        setPeerHolding(peerId, snapshot["ownership"]?.stringValue == "held"
                       && (ours == nil || snapshot["headsetId"]?.stringValue == ours))
        if unknownAfterDrop?.requesterId == peerId { unknownAfterDrop = nil }
        if let r = coordinator.live, r.requesterId == peerId {
            let held = snapshot["ownership"]?.stringValue == "held" && snapshot["headsetId"]?.stringValue == r.headsetId
            let named = snapshot["acquisition"]?.objectValue?["requestId"]?.stringValue
            if held {
                coordinator.participantResolve(requestId: r.requestId, reason: "requester_reports_held")
            } else if r.phase == .inProgress, named != r.requestId {
                coordinator.participantResolve(requestId: r.requestId, reason: "requester_acquisition_ended")
            }
        }
        return true
    }

    // MARK: Acquisition

    private func handleAcquisition(_ link: Link, _ remoteSessionId: String, _ message: PeerV2Message.Validated) throws {
        let name = peerName(link.peerId)
        switch message.kind {
        case .acquireGrant, .acquireDeny, .acquireStartAck:
            // Answers to requests this computer never makes.
            return log("V2[\(name)]: \(message.kind) ignored (this computer never requests)")
        default:
            break
        }
        // Membership is the configured peers plus this computer; unknown IDs make every request a mismatch.
        let peers = configuredPeerIds()
        let configured = peers.map { $0.union([installationId]) } ?? []
        let status = localStatus()
        var current: [String: Int64]?
        if message.kind == .acquireRequest {
            current = (message.fields["snapshotRevisions"]?.objectValue ?? [:]).compactMapValues(\.intValue)
        } else {
            current = coordinator.reservation?.snapshotRevisions
        }
        if current != nil, !current!.isEmpty { current![installationId] = status.revision }
        let frames = try protocolHandler.receive(
            authenticatedPeerId: link.peerId, authenticatedPeerSessionId: remoteSessionId, message: message,
            localStatus: status, configuredMembership: configured, currentSnapshotRevisions: current)
        let request = String((message.fields["requestId"]?.stringValue ?? "").prefix(8))
        var outcomes: [String] = []
        for frame in frames {
            if let decoded = try? PeerV2Message.decode(frame) {
                outcomes.append(decoded.kind == .acquireDeny ? "DENY \(decoded.fields["reason"]?.stringValue ?? "?")" : "\(decoded.kind)")
            }
            if !link.send(frame) { throw V2Error("v2_writer_unavailable") }
        }
        let state = localState()
        log("V2[\(name)]: \(message.kind) req \(request) -> \(outcomes.isEmpty ? "no reply" : outcomes.joined(separator: ", ")) "
            + "(ownership \(measuredOwnershipText()), audio \(audioUseText(state.audioUse)), rev \(status.revision)"
            + (peers == nil ? ", membership unknown" : "") + ")")
        flush()
    }

    // MARK: Reservation enforcement

    /// True while a grant is live (PREPARING, STARTING, IN_PROGRESS): no automatic takeover may start.
    var reservationLive: Bool { coordinator.live != nil }

    /// The user explicitly asked to connect the AirPods here: the reservation yields.
    func cancelReservationForUserTakeover() {
        if coordinator.cancelForLocalTakeover() { log("V2: reservation cancelled; the user asked to connect the AirPods here") }
        flush()
    }

    private func reservationChanged(_ requestId: String, _ phase: ReservationPhase, _ reason: String) {
        log("V2: reservation \(requestId.prefix(8)) \(phase) (\(reason))")
        if phase == .resolved || phase == .cancelled {
            DispatchQueue.main.async { [weak self] in
                self?.flush()
                self?.onReservationEnded?()
            }
        }
    }
}
