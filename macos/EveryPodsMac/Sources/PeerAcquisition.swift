// PeerAcquisition.swift
//
// The grantor (participant) side of passive acquisition ("acquire-v1"): a phone that wants the AirPods when its lid
// opens asks every configured peer to GRANT, and connects only after every peer ACKs START. Computers only vote;
// they never request. A port of the participant half of Android's PassiveAcquisitionCoordinator.kt and of
// PeerAcquisitionProtocol.kt (same decisions, same denial reasons, EXPIRED goes out as "UNKNOWN").
// Pure Swift, no Bluetooth: Tests/run.sh builds it with PeerV2.swift.

import Foundation

enum PeerOwnership: Equatable { case unknown, held, released }
enum PeerAudioUse: Equatable { case unknown, idle, media, call }

enum Denial: String {
    case held = "HELD", unknown = "UNKNOWN", membershipMismatch = "MEMBERSHIP_MISMATCH", busy = "BUSY"
    case staleSnapshot = "STALE_SNAPSHOT", sessionChanged = "SESSION_CHANGED", expired = "EXPIRED"
    /// EXPIRED is internal; the wire schema has no such reason.
    var wireValue: String { self == .expired ? "UNKNOWN" : rawValue }
}

enum ReservationPhase: Equatable { case preparing, starting, inProgress, resolved, cancelled }

/// What this computer reports about itself (Android's PeerStatus).
struct LocalStatus: Equatable {
    var installationId: String
    var sessionId: String
    var revision: Int64
    var ready = true
    var headsetId: String?
    var ownership: PeerOwnership
    var audioUse: PeerAudioUse
}

struct ParticipantReservation: Equatable {
    var requestId: String
    var requesterId: String
    var requesterSessionId: String
    var headsetId: String
    var membershipDigest: String
    var snapshotRevisions: [String: Int64]
    var grantId: String
    var grantorSessionId: String
    var phase: ReservationPhase
    var expiresAtMs: Int64

    var isLive: Bool { phase == .preparing || phase == .starting || phase == .inProgress }
}

enum ParticipantEvent: Equatable {
    case grant(requestId: String, grantId: String)
    case deny(requestId: String, reason: Denial)
    case startAck(requestId: String, grantId: String)
}

/// One reservation at a time, never any headset command.
final class ParticipantCoordinator {
    static let preparationTTLMs: Int64 = 5_000
    /// Android keeps IN_PROGRESS until the requester's terminal CANCEL; Windows bounds it at 60 s so that a lost
    /// CANCEL cannot block this computer's takeovers forever. Same here.
    static let inProgressSafetyMs: Int64 = 60_000

    private let localInstallationId: String
    private let clock: () -> Int64
    private let idFactory: () -> String
    private(set) var reservation: ParticipantReservation?
    /// The request whose PREPARING grant `expire()` cancelled: a late repeat of it is answered EXPIRED, as the
    /// Kotlin does when it notices the expiry itself.
    private var expiredRequestId: String?
    /// (requestId, phase, reason) on every change, for the log and the snapshot.
    var onTransition: ((String, ReservationPhase, String) -> Void)?

    init(localInstallationId: String, clock: @escaping () -> Int64,
         idFactory: @escaping () -> String = { UUID().uuidString.lowercased() }) {
        self.localInstallationId = localInstallationId.lowercased()
        self.clock = clock
        self.idFactory = idFactory
    }

    private func changed(_ requestId: String, _ phase: ReservationPhase, _ reason: String) {
        onTransition?(requestId, phase, reason)
    }

    /// The live reservation, if any (an expired PREPARING one is released first).
    var live: ParticipantReservation? {
        expire()
        return reservation?.isLive == true ? reservation : nil
    }

    /// Android notices an expired PREPARING grant lazily, on the next request or START. A computer must not hold a
    /// takeover back on a dead grant, so the tick calls this too.
    func expire() {
        guard let r = reservation, r.phase == .preparing || r.phase == .inProgress, clock() >= r.expiresAtMs else { return }
        reservation = ParticipantReservation.with(r, phase: .cancelled)
        if r.phase == .preparing { expiredRequestId = r.requestId }
        changed(r.requestId, .cancelled, r.phase == .preparing ? "preparation_expired" : "in_progress_safety_expired")
    }

    func participantRequest(
        requestId: String, requesterId: String, requesterSessionId: String, headsetId: String,
        membership: [String], membershipDigest: String, localSessionId: String,
        localStatus: LocalStatus?, configuredMembership: Set<String>,
        snapshotRevisions: [String: Int64]?, currentSnapshotRevisions: [String: Int64]?
    ) -> ParticipantEvent {
        let now = clock()
        let members = membership.map { $0.lowercased() }
        // Computers don't link to each other, so membership may name peers this Mac doesn't know. It must contain every
        // peer this Mac does know (each of them was asked), plus this Mac and the requester.
        let configured = Set(configuredMembership.map { $0.lowercased() })
        if !configured.contains(localInstallationId) || !configured.isSubset(of: Set(members)) || members != Array(Set(members)).sorted() || !members.contains(localInstallationId) ||
            !members.contains(requesterId.lowercased()) || PeerV2Message.membershipDigest(members) != membershipDigest {
            return .deny(requestId: requestId, reason: .membershipMismatch)
        }
        if let revisions = snapshotRevisions,
           currentSnapshotRevisions == nil || Set(revisions.keys) != Set(members) ||
            !PeerAcquisitionRules.revisionsMatchKnown(revisions, currentSnapshotRevisions!) {
            return .deny(requestId: requestId, reason: .staleSnapshot)
        }
        guard let status = localStatus, status.ready, status.sessionId == localSessionId, status.headsetId == headsetId,
              status.ownership != .unknown else { return .deny(requestId: requestId, reason: .unknown) }
        if status.ownership == .held || status.audioUse != .idle { return .deny(requestId: requestId, reason: .held) }
        if let existing = reservation, existing.isLive {
            if existing.phase == .preparing, now >= existing.expiresAtMs {
                reservation = ParticipantReservation.with(existing, phase: .cancelled)
                if existing.requestId == requestId { return .deny(requestId: requestId, reason: .expired) }
            } else if existing.requestId == requestId, existing.requesterId == requesterId,
                      existing.requesterSessionId == requesterSessionId, existing.headsetId == headsetId,
                      existing.membershipDigest == membershipDigest {
                return .grant(requestId: requestId, grantId: existing.grantId)
            } else {
                return .deny(requestId: requestId, reason: .busy)
            }
        }
        if expiredRequestId == requestId { return .deny(requestId: requestId, reason: .expired) }
        let grantId = idFactory()
        reservation = ParticipantReservation(
            requestId: requestId, requesterId: requesterId, requesterSessionId: requesterSessionId, headsetId: headsetId,
            membershipDigest: membershipDigest, snapshotRevisions: snapshotRevisions ?? [:], grantId: grantId,
            grantorSessionId: localSessionId, phase: .preparing, expiresAtMs: now + Self.preparationTTLMs)
        changed(requestId, .preparing, "granted")
        return .grant(requestId: requestId, grantId: grantId)
    }

    func participantStart(
        requestId: String, requesterId: String, requesterSessionId: String, grantId: String, currentSessionId: String,
        localStatus: LocalStatus?, currentSnapshotRevisions: [String: Int64]?,
        membership: [String]?, configuredMembership: Set<String>?
    ) -> ParticipantEvent {
        guard let current = reservation else { return .deny(requestId: requestId, reason: .expired) }
        if current.requestId != requestId || current.requesterId != requesterId ||
            current.requesterSessionId != requesterSessionId || current.grantId != grantId ||
            currentSessionId != current.grantorSessionId {
            return .deny(requestId: requestId, reason: .expired)
        }
        let duplicateStart = current.phase == .inProgress
        if !duplicateStart, current.phase != .preparing || clock() >= current.expiresAtMs {
            if current.phase == .preparing { reservation = ParticipantReservation.with(current, phase: .cancelled) }
            return .deny(requestId: requestId, reason: .expired)
        }
        if !current.snapshotRevisions.isEmpty,
           !PeerAcquisitionRules.revisionsMatchKnown(current.snapshotRevisions, currentSnapshotRevisions ?? [:]) {
            if !duplicateStart { reservation = ParticipantReservation.with(current, phase: .cancelled) }
            return .deny(requestId: requestId, reason: .staleSnapshot)
        }
        if let members = membership {
            let configured = configuredMembership.map { Set($0.map { $0.lowercased() }) }
            if members != Array(Set(members)).sorted() || PeerV2Message.membershipDigest(members) != current.membershipDigest ||
                !(configured ?? []).isSubset(of: Set(members.map { $0.lowercased() })) {
                if !duplicateStart { reservation = ParticipantReservation.with(current, phase: .cancelled) }
                return .deny(requestId: requestId, reason: .membershipMismatch)
            }
        }
        if let status = localStatus,
           !status.ready || status.sessionId != currentSessionId || status.headsetId != current.headsetId ||
            status.ownership != .released || status.audioUse != .idle {
            if !duplicateStart { reservation = ParticipantReservation.with(current, phase: .cancelled) }
            return .deny(requestId: requestId, reason: status.ownership == .held || status.audioUse != .idle ? .held : .unknown)
        }
        if duplicateStart { return .startAck(requestId: requestId, grantId: grantId) }
        reservation = ParticipantReservation.with(current, phase: .inProgress, expiresAtMs: clock() + Self.inProgressSafetyMs)
        changed(current.requestId, .inProgress, "start_acknowledged")
        return .startAck(requestId: requestId, grantId: grantId)
    }

    /// CANCEL is terminal proof from the authenticated requester, not a timeout.
    @discardableResult
    func participantCancel(requestId: String, requesterId: String, requesterSessionId: String, membershipDigest: String?) -> Bool {
        guard let current = reservation, current.requestId == requestId, current.requesterId == requesterId,
              current.requesterSessionId == requesterSessionId,
              membershipDigest == nil || current.membershipDigest == membershipDigest, current.isLive else { return false }
        reservation = ParticipantReservation.with(current, phase: .cancelled)
        changed(requestId, .cancelled, "requester_terminal_cancel")
        return true
    }

    @discardableResult
    func participantResolve(requestId: String, reason: String) -> Bool {
        guard let current = reservation, current.requestId == requestId, current.isLive else { return false }
        reservation = ParticipantReservation.with(current, phase: .resolved)
        changed(requestId, .resolved, reason)
        return true
    }

    /// The user explicitly took the AirPods on this computer: the reservation yields (the requester's START is then
    /// denied, and the takeover asks the holder to release as usual).
    @discardableResult
    func cancelForLocalTakeover() -> Bool {
        guard let current = reservation, current.isLive else { return false }
        reservation = ParticipantReservation.with(current, phase: .cancelled)
        changed(current.requestId, .cancelled, "local_takeover_by_user")
        return true
    }
}

extension ParticipantReservation {
    static func with(_ r: ParticipantReservation, phase: ReservationPhase, expiresAtMs: Int64? = nil) -> ParticipantReservation {
        var copy = r
        copy.phase = phase
        if let expiresAtMs { copy.expiresAtMs = expiresAtMs }
        return copy
    }
}

/// Revisions advance only when the measured state changes; reservation bookkeeping (`acquisition`) is excluded so
/// that creating a reservation does not invalidate the revisions it just reserved (PeerSnapshotRevision.kt).
final class PeerSnapshotRevision {
    private var last: [String: JSONValue]?
    private var revision: Int64 = 0

    func forState(_ state: [String: JSONValue]) -> Int64 {
        var ownership = state
        ownership["acquisition"] = nil
        if last != ownership {
            revision += 1
            last = ownership
        }
        return revision
    }
}

/// Turns v2 acquisition messages into replies (PeerAcquisitionProtocol.receive for the participant messages).
struct PeerAcquisitionProtocol {
    let localInstallationId: String
    let localSessionId: () -> String
    let coordinator: ParticipantCoordinator

    /// `authenticatedPeerId`/`SessionId` come from the peer's HELLO on this link. Throws (ending the session, as
    /// on Android) when the message's identity does not match. REQUEST, START and CANCEL are the only messages a
    /// computer acts on; GRANT, DENY and START_ACK are answers to requests this computer never makes.
    func receive(
        authenticatedPeerId: String, authenticatedPeerSessionId: String, message: PeerV2Message.Validated,
        localStatus: LocalStatus?, configuredMembership: Set<String>, currentSnapshotRevisions: [String: Int64]?
    ) throws -> [PeerV2Frame.Frame] {
        let f = message.fields
        func str(_ key: String) -> String { f[key]?.stringValue ?? "" }
        let members = (f["membership"]?.arrayValue ?? []).compactMap(\.stringValue)
        switch message.kind {
        case .acquireRequest:
            guard str("requesterId") == authenticatedPeerId, str("requesterSessionId") == authenticatedPeerSessionId else {
                throw V2Error("requester_identity_mismatch")
            }
            let revisions = (f["snapshotRevisions"]?.objectValue ?? [:]).compactMapValues(\.intValue)
            let decision = coordinator.participantRequest(
                requestId: str("requestId"), requesterId: authenticatedPeerId, requesterSessionId: authenticatedPeerSessionId,
                headsetId: str("headsetId"), membership: members, membershipDigest: str("membershipDigest"),
                localSessionId: localSessionId(), localStatus: localStatus, configuredMembership: configuredMembership,
                snapshotRevisions: revisions, currentSnapshotRevisions: currentSnapshotRevisions)
            return [try reply(decision, request: f)]
        case .acquireStart:
            guard str("requesterId") == authenticatedPeerId, str("requesterSessionId") == authenticatedPeerSessionId,
                  str("grantorId") == localInstallationId, str("grantorSessionId") == localSessionId() else {
                throw V2Error("start_identity_mismatch")
            }
            let decision = coordinator.participantStart(
                requestId: str("requestId"), requesterId: authenticatedPeerId, requesterSessionId: authenticatedPeerSessionId,
                grantId: str("grantId"), currentSessionId: localSessionId(), localStatus: localStatus,
                currentSnapshotRevisions: currentSnapshotRevisions, membership: members, configuredMembership: configuredMembership)
            return [try reply(decision, request: f)]
        case .acquireCancel:
            guard str("requesterId") == authenticatedPeerId, str("requesterSessionId") == authenticatedPeerSessionId else {
                throw V2Error("cancel_identity_mismatch")
            }
            coordinator.participantCancel(requestId: str("requestId"), requesterId: authenticatedPeerId,
                                          requesterSessionId: authenticatedPeerSessionId, membershipDigest: str("membershipDigest"))
            return []
        default:
            throw V2Error("not_acquisition_message")
        }
    }

    private func reply(_ event: ParticipantEvent, request f: [String: JSONValue]) throws -> PeerV2Frame.Frame {
        var fields: [String: JSONValue] = [:]
        for key in ["requestId", "headsetId", "requesterId", "requesterSessionId", "membership", "membershipDigest"] {
            fields[key] = f[key]
        }
        fields["grantorId"] = .string(localInstallationId)
        fields["grantorSessionId"] = .string(localSessionId())
        switch event {
        case .grant(_, let grantId):
            fields["grantId"] = .string(grantId)
            return try PeerV2Message.encode(.acquireGrant, fields)
        case .startAck(_, let grantId):
            fields["grantId"] = .string(grantId)
            return try PeerV2Message.encode(.acquireStartAck, fields)
        case .deny(_, let reason):
            fields["reason"] = .string(reason.wireValue)
            return try PeerV2Message.encode(.acquireDeny, fields)
        }
    }
}


/// Membership rules for a mesh where computers don't link to each other.
enum PeerAcquisitionRules {
    /// Every revision this device knows must match the requester's; revisions it can't see are not checked.
    static func revisionsMatchKnown(_ theirs: [String: Int64], _ known: [String: Int64]) -> Bool {
        known.allSatisfy { theirs[$0.key] == $0.value }
    }
}
