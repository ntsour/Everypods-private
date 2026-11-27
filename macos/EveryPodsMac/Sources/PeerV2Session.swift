// PeerV2Session.swift
//
// The v2 protocol (HELLO / SNAPSHOT / PING / PONG / ACQUIRE_*) over one Bluetooth LE link, as the LE central: the
// initiator. A port of Android's PeerLeV2Session.kt with `central = true`, PeerHeartbeat.kt, and the
// 5 s handshake watchdog. Every v2 record carries exactly one frame. Everything outside the protocol (identity,
// snapshots, the wire) is injected through `Hooks`, and time through `clock`, so the tests drive it directly.
// A failure ends the session once and reports through `onEnded` (the LE link stays up; the owner starts over).

import Foundation

/// Nonce-correlated heartbeat: two missed response intervals mean the peer is gone.
struct PeerHeartbeat {
    enum Action: Equatable { case sendPing(String), awaitResponse, timedOut }
    static let maxMissedIntervals = 2

    private var outstandingNonce: String?
    private var missedIntervals = 0
    private let nonceFactory: () -> String

    init(nonceFactory: @escaping () -> String = { UUID().uuidString.lowercased() }) { self.nonceFactory = nonceFactory }

    mutating func startPing() -> String? {
        guard outstandingNonce == nil else { return nil }
        let nonce = nonceFactory()
        outstandingNonce = nonce
        return nonce
    }

    mutating func onInterval() -> Action {
        if outstandingNonce != nil {
            missedIntervals += 1
            return missedIntervals >= Self.maxMissedIntervals ? .timedOut : .awaitResponse
        }
        return .sendPing(startPing()!)
    }

    /// The PING still waiting for its PONG, if any.
    var outstanding: String? { outstandingNonce }

    mutating func acknowledge(_ nonce: String) -> Bool {
        guard nonce == outstandingNonce else { return false }
        outstandingNonce = nil
        missedIntervals = 0
        return true
    }
}

protocol PeerV2SessionHooks: AnyObject {
    /// Sends one v2 record carrying `frame`; false if the link cannot take it.
    func send(_ frame: PeerV2Frame.Frame) -> Bool
    /// True while the peer is a configured peer (checked for every frame).
    func isAuthorized() -> Bool
    func localInstallationId() -> String
    func localSessionId() -> String
    func localSnapshot() -> [String: JSONValue]
    func newConnectionId() -> String
    /// The installation ID the peer's HELLO claims. Throw to refuse it.
    func bindPeerIdentity(_ installationId: String) throws
    /// A peer SNAPSHOT (also inside a PONG); false rejects it and ends the session.
    func observeSnapshot(remoteSessionId: String, snapshot: [String: JSONValue]) -> Bool
    func markReady(remoteSessionId: String, acquisitionCapable: Bool)
    func handleAcquisition(remoteSessionId: String, message: PeerV2Message.Validated) throws
    /// The session ended (`reason` is a short code); called at most once, never after `end()`.
    func onEnded(reason: String)
}

final class PeerV2Session {
    static let heartbeatMs: Int64 = 10_000
    static let handshakeTimeoutMs: Int64 = 5_000
    /// quiet-v1 peers: PING only after this long with nothing received, then again after `probeRetryMs`; two
    /// unanswered probes end the session (docs/peer-link-quiet-redesign.md, D1).
    static let idleProbeMs: Int64 = 60_000
    static let probeRetryMs: Int64 = 15_000

    private unowned let hooks: PeerV2SessionHooks
    private let clock: () -> Int64
    private var decoder = PeerV2Frame.Decoder()
    private var heartbeat = PeerHeartbeat()
    private var connectionId: String?
    private var remoteInstall: String?
    private var remoteSession: String?
    private var remoteAcquisitionCapable = false
    private var initialSnapshot = false
    private var initialPong = false
    private var handshakeDeadlineMs: Int64?
    private var nextHeartbeatMs: Int64
    private var remoteQuiet = false
    private var lastReceivedMs: Int64
    private var probeSentMs: Int64?
    private var probesUnanswered = 0
    private(set) var isEnded = false
    private(set) var isReady = false

    init(hooks: PeerV2SessionHooks, clock: @escaping () -> Int64) {
        self.hooks = hooks
        self.clock = clock
        nextHeartbeatMs = clock() + Self.heartbeatMs
        lastReceivedMs = clock()
    }

    /// Sends HELLO and arms the handshake watchdog.
    func start() {
        guard !isEnded else { return }
        guarded {
            let install = hooks.localInstallationId()
            let id = hooks.newConnectionId()
            connectionId = id
            handshakeDeadlineMs = clock() + Self.handshakeTimeoutMs
            try send(.hello, [
                "installationId": .string(install), "sessionId": .string(hooks.localSessionId()),
                "capabilities": .array(PeerV2Message.capabilities.map { .string($0) }),
                "initiatorId": .string(install), "connectionId": .string(id),
            ])
        }
    }

    /// One v2 record payload (exactly one frame).
    func onRecord(_ payload: Data) {
        guard !isEnded else { return }
        lastReceivedMs = clock()
        guarded {
            let frames = try decoder.feed(payload)
            guard frames.count == 1 else { throw V2Error("v2_record_not_one_frame") }
            try handle(frames[0])
        }
        // Any frame proves a quiet-v1 peer alive. The probe's nonce stays until its PONG arrives, so a PONG that comes
        // after another frame (both sides probing at once) still delivers its snapshot.
        if isReady && remoteQuiet {
            probeSentMs = nil
            probesUnanswered = 0
        }
    }

    /// Call about once a second: runs the handshake watchdog and the heartbeat (10 s, or the idle probe toward a
    /// quiet-v1 peer).
    func tick() {
        guard !isEnded else { return }
        let now = clock()
        guarded {
            if let deadline = handshakeDeadlineMs, !initialPong, now >= deadline { throw V2Error("handshake_timeout") }
            if isReady && remoteQuiet {
                if let sent = probeSentMs {
                    guard now - sent >= Self.probeRetryMs else { return }
                    if probesUnanswered >= 2 { throw V2Error("heartbeat_timeout") }
                } else {
                    guard now - lastReceivedMs >= Self.idleProbeMs else { return }
                }
                let nonce = heartbeat.outstanding ?? heartbeat.startPing()!
                try send(.ping, ["nonce": .string(nonce)])
                probeSentMs = now
                probesUnanswered += 1
                return
            }
            guard now >= nextHeartbeatMs else { return }
            nextHeartbeatMs = now + Self.heartbeatMs
            guard remoteInstall != nil else { return }
            switch heartbeat.onInterval() {
            case .sendPing(let nonce): try send(.ping, ["nonce": .string(nonce)])
            case .awaitResponse: break
            case .timedOut: throw V2Error("heartbeat_timeout")
            }
        }
    }

    /// Sends a frame to the peer if the session is ready.
    @discardableResult
    func sendFrame(_ frame: PeerV2Frame.Frame) -> Bool {
        guard isReady, !isEnded else { return false }
        return hooks.send(frame)
    }

    /// Ends the session without calling `onEnded` (the owner is tearing it down).
    func end() { isEnded = true; isReady = false }

    private func handle(_ frame: PeerV2Frame.Frame) throws {
        guard hooks.isAuthorized() else { throw V2Error("unauthorized_coordination_peer") }
        let message = try PeerV2Message.decode(frame)
        let fields = message.fields
        switch message.kind {
        case .hello:
            guard remoteInstall == nil else { throw V2Error("duplicate_hello") }
            guard fields["initiatorId"]?.stringValue == hooks.localInstallationId(),
                  fields["connectionId"]?.stringValue == connectionId else { throw V2Error("v2_connection_identity_mismatch") }
            let install = fields["installationId"]?.stringValue ?? ""
            try hooks.bindPeerIdentity(install)
            remoteInstall = install
            remoteSession = fields["sessionId"]?.stringValue
            let caps = (fields["capabilities"]?.arrayValue ?? []).compactMap(\.stringValue)
            remoteAcquisitionCapable = caps.contains("acquire-v1")
            remoteQuiet = caps.contains("quiet-v1")
        case .snapshot:
            guard let session = remoteSession else { throw V2Error("hello_required") }
            guard fields["sessionId"]?.stringValue == session else { throw V2Error("v2_session_mismatch") }
            guard hooks.observeSnapshot(remoteSessionId: session, snapshot: fields) else { throw V2Error("v2_snapshot_not_authoritative") }
            if !initialSnapshot {
                initialSnapshot = true
                try send(.snapshot, hooks.localSnapshot())
                guard let nonce = heartbeat.startPing() else { throw V2Error("initial_ping_already_pending") }
                try send(.ping, ["nonce": .string(nonce)])
            }
        case .ping:
            // The PONG carries the snapshot; a separate SNAPSHOT is the same data twice once the session is ready.
            let snapshot = hooks.localSnapshot()
            if !isReady { try send(.snapshot, snapshot) }
            try send(.pong, ["nonce": fields["nonce"] ?? .null, "snapshot": .object(snapshot)])
        case .pong:
            guard let nonce = fields["nonce"]?.stringValue else { throw V2Error("invalid_nonce") }
            if heartbeat.acknowledge(nonce) {
                guard let session = remoteSession else { throw V2Error("hello_required") }
                guard let snapshot = fields["snapshot"]?.objectValue else { throw V2Error("invalid_v2_snapshot") }
                guard snapshot["sessionId"]?.stringValue == session else { throw V2Error("v2_session_mismatch") }
                guard hooks.observeSnapshot(remoteSessionId: session, snapshot: snapshot) else {
                    throw V2Error("v2_pong_snapshot_not_authoritative")
                }
                initialPong = true
                isReady = true
                hooks.markReady(remoteSessionId: session, acquisitionCapable: remoteAcquisitionCapable)
            }
        case .acquireRequest, .acquireGrant, .acquireDeny, .acquireCancel, .acquireStart, .acquireStartAck:
            guard remoteAcquisitionCapable else { throw V2Error("peer_missing_acquire_v1") }
            guard let session = remoteSession else { throw V2Error("hello_required") }
            try hooks.handleAcquisition(remoteSessionId: session, message: message)
        case .relayData:
            throw V2Error("unsupported_v2_message")
        }
    }

    private func send(_ kind: PeerV2Frame.Kind, _ fields: [String: JSONValue]) throws {
        guard hooks.send(try PeerV2Message.encode(kind, fields)) else { throw V2Error("v2_writer_unavailable") }
    }

    private func guarded(_ block: () throws -> Void) {
        do { try block() } catch { fail((error as? V2Error)?.code ?? "\(error)") }
    }

    private func fail(_ reason: String) {
        guard !isEnded else { return }
        isEnded = true
        isReady = false
        hooks.onEnded(reason: reason)
    }
}
