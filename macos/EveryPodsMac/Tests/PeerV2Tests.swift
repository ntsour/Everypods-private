// PeerV2Tests.swift — run with Tests/run.sh. Ports the codec, identity and participant cases of Android's
// PeerV2FrameTest, PeerV2MessageTest, PeerIdentityStoreTest, PeerSnapshotRevisionTest,
// PassiveAcquisitionCoordinatorTest and PeerAcquisitionProtocolTest, plus the central handshake of PeerLeV2SessionTest.

import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ message: String, line: Int = #line) {
    if !condition() { failures += 1; print("FAIL line \(line): \(message)") }
}
func throwsCode(_ code: String, _ body: () throws -> Void) -> Bool {
    do { try body() } catch let e as V2Error { return e.code == code } catch { return false }
    return false
}

let a = "00000000-0000-4000-8000-000000000001"
let b = "00000000-0000-4000-8000-000000000002"
let c = "00000000-0000-4000-8000-000000000003"
let headset = "00000000-0000-4000-8000-000000000099"
let sessA = "00000000-0000-4000-8000-0000000000a1"
let sessB = "00000000-0000-4000-8000-0000000000b1"

// MARK: Frame codec (PeerV2FrameTest)

let ping = try! PeerV2Frame.Frame(kind: .ping, payload: Data()).encode()
check(Array(ping) == [0x45, 0x50, 0x48, 0x32, 2, 3, 0, 0, 0, 0], "empty PING golden frame")
check(PeerV2Frame.Kind.allCases.map(\.rawValue) == Array(1 ... 11), "type ids 1...11")
for kind in PeerV2Frame.Kind.allCases {
    let original = try! PeerV2Frame.Frame(kind: kind, payload: Data([0, 1, 2, 3, 4, 5])).encode()
    for split in 0 ... original.count {
        var decoder = PeerV2Frame.Decoder()
        let result = (try! decoder.feed(original.prefix(split))) + (try! decoder.feed(original.suffix(from: split)))
        check(result.count == 1 && result[0].kind == kind && result[0].payload == Data([0, 1, 2, 3, 4, 5]), "split \(kind) \(split)")
        check(decoder.bufferedByteCount == 0, "no leftover \(kind) \(split)")
    }
}
var coalesced = PeerV2Frame.Decoder()
let both = try! PeerV2Frame.Frame(kind: .ping, payload: Data([1])).encode() + PeerV2Frame.Frame(kind: .pong, payload: Data([2])).encode()
check((try! coalesced.feed(both)).map(\.kind) == [.ping, .pong], "coalesced frames")
var bad = PeerV2Frame.Decoder()
check(throwsCode("invalid_magic") { _ = try bad.feed(Data("XPH2".utf8) + Data(count: 6)) }, "bad magic")
bad = PeerV2Frame.Decoder()
check(throwsCode("unsupported_version") { _ = try bad.feed(Data("EPH2".utf8) + Data([3, 1, 0, 0, 0, 0])) }, "bad version")
bad = PeerV2Frame.Decoder()
check(throwsCode("unknown_type") { _ = try bad.feed(Data("EPH2".utf8) + Data([2, 12, 0, 0, 0, 0])) }, "unknown type")
bad = PeerV2Frame.Decoder()
check(throwsCode("oversized_payload") { _ = try bad.feed(Data("EPH2".utf8) + Data([2, 1, 0, 0, 0x10, 0x01])) }, "oversized payload")
check(throwsCode("oversized_payload") { _ = try PeerV2Frame.Frame(kind: .relayData, payload: Data(count: 4097)).encode() }, "oversized encode")

// MARK: Message validation (PeerV2MessageTest)

func frame(_ kind: PeerV2Frame.Kind, _ json: String) -> PeerV2Frame.Frame { PeerV2Frame.Frame(kind: kind, payload: Data(json.utf8)) }
let id1 = "00000000-0000-4000-8000-000000000011", id2 = "00000000-0000-4000-8000-000000000012"
check((try? PeerV2Message.decode(frame(.ping, #"{"nonce":"\#(id1)"}"#))) != nil, "valid PING")
check(throwsCode("invalid_fields") { _ = try PeerV2Message.decode(frame(.ping, #"{"nonce":"\#(id1)","extra":true}"#)) }, "unknown field")
check(throwsCode("duplicate_json_key") { _ = try PeerV2Message.decode(frame(.ping, #"{"nonce":"\#(id1)","nonce":"\#(id2)"}"#)) }, "duplicate key")
check(throwsCode("invalid_nonce") { _ = try PeerV2Message.decode(frame(.ping, #"{"nonce":"not-a-uuid"}"#)) }, "bad uuid")
check(throwsCode("invalid_nonce") { _ = try PeerV2Message.decode(frame(.ping, #"{"nonce":"\#(sessA.uppercased())"}"#)) }, "uppercase uuid")
check(throwsCode("trailing_json") { _ = try PeerV2Message.decode(frame(.ping, #"{"nonce":"\#(id1)"} false"#)) }, "trailing json")
check(throwsCode("json_too_deep") { _ = try PeerV2Message.decode(frame(.ping, String(repeating: "[", count: 40) + String(repeating: "]", count: 40))) }, "deep json")
check(throwsCode("invalid_utf8") { _ = try PeerV2Message.decode(PeerV2Frame.Frame(kind: .ping, payload: Data([0xFF, 0xFE]))) }, "invalid utf8")
check(throwsCode("integer_required") { _ = try PeerV2Message.decode(frame(.snapshot, #"{"revision":1.5}"#)) }, "fraction refused")

let snapshotJson = #"{"sessionId":"\#(sessA)","revision":0,"headsetId":null,"ownership":"unknown","audioUse":"unknown","lastOwnerId":null,"lastOwnerRevision":null,"acquisition":null}"#
check((try? PeerV2Message.decode(frame(.snapshot, snapshotJson))) != nil, "documented SNAPSHOT example")
check(throwsCode("invalid_last_owner") { _ = try PeerV2Message.decode(frame(.snapshot, snapshotJson.replacingOccurrences(of: #""lastOwnerRevision":null"#, with: #""lastOwnerRevision":3"#))) }, "owner revision without owner")
check(throwsCode("invalid_ownership") { _ = try PeerV2Message.decode(frame(.snapshot, snapshotJson.replacingOccurrences(of: "unknown\",\"audioUse", with: "HELD\",\"audioUse"))) }, "case-sensitive enum")
check(throwsCode("invalid_acquisition_phase") { _ = try PeerV2Message.decode(frame(.snapshot, snapshotJson.replacingOccurrences(of: #""acquisition":null"#, with: #""acquisition":{"requestId":"\#(id1)","requesterId":"\#(a)","phase":"starting"}"#))) }, "acquisition phase")
check((try? PeerV2Message.decode(frame(.snapshot, snapshotJson.replacingOccurrences(of: #""acquisition":null"#, with: #""acquisition":{"requestId":"\#(id1)","requesterId":"\#(a)","phase":"in_progress"}"#)))) != nil, "acquisition in_progress")
// Integers beyond 2^53 survive (Long, not Double).
let big = try! PeerV2Message.decode(frame(.snapshot, snapshotJson.replacingOccurrences(of: #""revision":0"#, with: #""revision":9007199254740993"#)))
check(big.fields["revision"]?.intValue == 9_007_199_254_740_993, "revision is a 64-bit integer")

// Encode round trip and exact bytes.
let pingFrame = try! PeerV2Message.encode(.ping, ["nonce": .string(id1)])
check(String(data: pingFrame.payload, encoding: .utf8) == #"{"nonce":"\#(id1)"}"#, "compact JSON")
let helloFields: [String: JSONValue] = ["installationId": .string(a), "sessionId": .string(sessA),
    "capabilities": .array([.string("acquire-v1"), .string("snapshot")]), "initiatorId": .string(a), "connectionId": .string(id1)]
let helloFrame = try! PeerV2Message.encode(.hello, helloFields)
check(try! PeerV2Message.decode(helloFrame).fields == helloFields, "HELLO round trip")
check(throwsCode("invalid_capabilities") { _ = try PeerV2Message.encode(.hello, helloFields.merging(["capabilities": .array([.string("snapshot"), .string("acquire-v1")])]) { $1 }) }, "capabilities must be sorted")
check((try? PeerV2Message.encode(.hello, helloFields.merging(["capabilities": .array([.string("relay")])]) { $1 })) != nil, "unknown capability names are accepted")
check(throwsCode("invalid_capabilities") { _ = try PeerV2Message.encode(.hello, helloFields.merging(["capabilities": .array([.string("Bad Name")])]) { $1 }) }, "malformed capability name")
check(String(data: helloFrame.payload, encoding: .utf8)!.contains(#""capabilities":["acquire-v1","snapshot"]"#), "HELLO advertises acquire-v1 and snapshot")
// Escapes: é and a surrogate pair decode; control characters are refused.
let escaped = try! PeerV2Message.decode(frame(.relayData, #"{"subtype":"anc","data":"AAE="}"#))
check(escaped.fields["data"]?.stringValue == "AAE=", "relay data")
check(throwsCode("invalid_control_character") { _ = try PeerV2Message.decode(frame(.ping, "{\"nonce\":\"a\nb\"}")) }, "raw control char")

// MARK: Membership digest and headset identity

check(PeerV2Message.membershipDigest([a, b]) == "d7c3edbc6845b7b42ab45e96b21182315e503144e9659129f16c5d2f03e88a79", "digest vector (sha256 of 'a\\nb\\n')")
check(PeerV2Message.membershipDigest([b, a, a.uppercased()]) == PeerV2Message.membershipDigest([a, b]), "digest sorts, lowercases and dedupes")
check(PeerV2Message.membershipDigest([a]) == "18cbd01ea85b5c51819e319666cb6a233d1136afe516b849cd61ae4348a98c15", "single-member digest")
check(try! PeerV2Message.headsetId(fromAddress: "AA:BB:CC:DD:EE:FF") == "261900fb-1113-8a4d-b748-173e13abd88a", "headsetId vector 1")
check(try! PeerV2Message.headsetId(fromAddress: "00:11:22:33:44:55") == "66fd6ef8-31e4-8c98-9570-30be94189a78", "headsetId vector 2")
check(try! PeerV2Message.headsetId(fromAddress: "aa:bb:cc:dd:ee:ff") == "261900fb-1113-8a4d-b748-173e13abd88a", "headsetId is case insensitive")
check(try! PeerV2Message.headsetId(fromAddress: "aa-bb-cc-dd-ee-ff") == "261900fb-1113-8a4d-b748-173e13abd88a", "headsetId accepts the dashed form")
check(throwsCode("invalid_bluetooth_address") { _ = try PeerV2Message.headsetId(fromAddress: "AirPods Pro 3") }, "headsetId rejects names")
check(PeerV2Message.isUUID(try! PeerV2Message.headsetId(fromAddress: "AA:BB:CC:DD:EE:FF")), "headsetId is a canonical UUID")

// MARK: Snapshot revision (PeerSnapshotRevisionTest)

let rev = PeerSnapshotRevision()
let released: [String: JSONValue] = ["ownership": .string("released"), "audioUse": .string("idle")]
let held: [String: JSONValue] = ["ownership": .string("held"), "audioUse": .string("media")]
check(rev.forState(released) == 1 && rev.forState(released) == 1, "unchanged state keeps the revision")
check(rev.forState(held) == 2 && rev.forState(held) == 2, "change increments once")
check(rev.forState(released) == 3, "change back increments")
check(rev.forState(released.merging(["acquisition": .object(["phase": .string("preparing")])]) { $1 }) == 3, "acquisition does not change the revision")

// MARK: Participant decisions (PassiveAcquisitionCoordinatorTest, PeerAcquisitionProtocolTest)

final class TestClock { var now: Int64 = 1_000 }

func status(_ ownership: PeerOwnership = .released, audio: PeerAudioUse = .idle, headsetId: String? = headset,
            revision: Int64 = 9, session: String = sessB) -> LocalStatus {
    LocalStatus(installationId: b, sessionId: session, revision: revision, ready: true, headsetId: headsetId, ownership: ownership, audioUse: audio)
}

func requestFields(id: String = "00000000-0000-4000-8000-0000000000e1", members: [String] = [a, b],
                   revisions: [String: Int64]? = nil, requester: String = a, requesterSession: String = sessA) -> [String: JSONValue] {
    let revs = revisions ?? Dictionary(uniqueKeysWithValues: members.map { ($0, Int64(9)) })
    return ["requestId": .string(id), "headsetId": .string(headset), "requesterId": .string(requester),
            "requesterSessionId": .string(requesterSession), "membership": .array(members.map { .string($0) }),
            "membershipDigest": .string(PeerV2Message.membershipDigest(members)),
            "snapshotRevisions": .object(revs.mapValues { .int($0) })]
}

func message(_ kind: PeerV2Frame.Kind, _ fields: [String: JSONValue]) -> PeerV2Message.Validated {
    try! PeerV2Message.decode(try! PeerV2Message.encode(kind, fields))
}

func startFields(from grant: PeerV2Message.Validated, members: [String] = [a, b]) -> [String: JSONValue] {
    var f = grant.fields
    f["grantorId"] = .string(b); f["grantorSessionId"] = .string(sessB)
    return f
}

struct Rig {
    let clock = TestClock()
    let coordinator: ParticipantCoordinator
    let proto: PeerAcquisitionProtocol
    init() {
        let clock = self.clock
        var n = 0
        coordinator = ParticipantCoordinator(localInstallationId: b, clock: { clock.now },
                                             idFactory: { n += 1; return String(format: "00000000-0000-4000-8000-0000000001%02d", n) })
        proto = PeerAcquisitionProtocol(localInstallationId: b, localSessionId: { sessB }, coordinator: coordinator)
    }
    func receive(_ m: PeerV2Message.Validated, status: LocalStatus? = status(), members: Set<String> = [a, b],
                 current: [String: Int64]? = [a: 9, b: 9], peer: String = a, peerSession: String = sessA) throws -> [PeerV2Message.Validated] {
        try proto.receive(authenticatedPeerId: peer, authenticatedPeerSessionId: peerSession, message: m, localStatus: status,
                          configuredMembership: members, currentSnapshotRevisions: current).map { try! PeerV2Message.decode($0) }
    }
}

func denyReason(_ replies: [PeerV2Message.Validated]) -> String? {
    guard replies.count == 1, replies[0].kind == .acquireDeny else { return nil }
    return replies[0].fields["reason"]?.stringValue
}

do {  // grant, start, ack; the reservation is reported in the snapshot until resolved
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    check(grant.count == 1 && grant[0].kind == .acquireGrant, "GRANT for a clean request")
    check(grant[0].fields["grantorId"]?.stringValue == b && grant[0].fields["grantorSessionId"]?.stringValue == sessB, "GRANT carries grantor identity")
    check(grant[0].fields["requestId"] == requestFields()["requestId"] && grant[0].fields["membershipDigest"] == requestFields()["membershipDigest"], "GRANT echoes the request identity")
    check(rig.coordinator.live?.phase == .preparing, "reservation PREPARING")
    check(rig.coordinator.reservation?.expiresAtMs == 6_000, "grant expires after 5 s")
    let duplicate = try! rig.receive(message(.acquireRequest, requestFields()))
    check(duplicate[0].fields == grant[0].fields, "duplicate REQUEST repeats the same grant")
    let ack = try! rig.receive(message(.acquireStart, startFields(from: grant[0])))
    check(ack.count == 1 && ack[0].kind == .acquireStartAck && ack[0].fields["grantId"] == grant[0].fields["grantId"], "START_ACK")
    check(rig.coordinator.live?.phase == .inProgress, "reservation IN_PROGRESS")
    let again = try! rig.receive(message(.acquireStart, startFields(from: grant[0])))
    check(again[0].fields == ack[0].fields, "duplicate START is idempotent")
    check(!rig.coordinator.participantResolve(requestId: "other", reason: "x"), "resolve needs the request")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields(id: "00000000-0000-4000-8000-0000000000e2")))) == "BUSY", "second request is BUSY")
    check(rig.coordinator.participantResolve(requestId: requestFields()["requestId"]!.stringValue!, reason: "test"), "resolve")
    check(rig.coordinator.live == nil, "resolved reservation is not live")
}

do {  // IN_PROGRESS bound (Windows safety bound; Android waits for CANCEL)
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    _ = try! rig.receive(message(.acquireStart, startFields(from: grant[0])))
    rig.clock.now += 59_999
    check(rig.coordinator.live?.phase == .inProgress, "IN_PROGRESS holds until the bound")
    rig.clock.now += 1
    check(rig.coordinator.live == nil && rig.coordinator.reservation?.phase == .cancelled, "IN_PROGRESS bounded at 60 s")
}

do {  // denial reasons
    let rig = Rig()
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(.held))) == "HELD", "held -> HELD")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(audio: .media))) == "HELD", "media audio -> HELD")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(audio: .call))) == "HELD", "call audio -> HELD")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(audio: .unknown))) == "HELD", "unobservable audio is not idle -> HELD")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(.unknown))) == "UNKNOWN", "unknown ownership -> UNKNOWN")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(headsetId: nil))) == "UNKNOWN", "no headset -> UNKNOWN")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: nil)) == "UNKNOWN", "no status -> UNKNOWN")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), status: status(headsetId: "00000000-0000-4000-8000-000000000098"))) == "UNKNOWN", "other headset -> UNKNOWN")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), members: [a, b, c])) == "MEMBERSHIP_MISMATCH", "unconfigured third member")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()), members: [])) == "MEMBERSHIP_MISMATCH", "membership unknown")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields(revisions: [a: 6, b: 9])), current: [a: 6, b: 10])) == "STALE_SNAPSHOT", "stale own revision")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields(revisions: [a: 6, b: 9])), current: nil)) == "STALE_SNAPSHOT", "no current revisions")
    check(rig.coordinator.live == nil, "denials create no reservation")
    // Computers don't link to each other: a request may name a peer this Mac doesn't know (e.g. the Windows PC).
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields(members: [a, b, c], revisions: [a: 6, b: 9, c: 1])), members: [a, b], current: [a: 6, b: 9])) == nil, "request may name a peer this Mac doesn't know")
    // A wrong digest never reaches the coordinator: the codec refuses it.
    var wrongDigest = requestFields(); wrongDigest["membershipDigest"] = .string(String(repeating: "a", count: 64))
    check(throwsCode("membership_digest_mismatch") { _ = try PeerV2Message.encode(.acquireRequest, wrongDigest) }, "digest mismatch refused by the codec")
}

do {  // identity checks end the session
    let rig = Rig()
    check(throwsCode("requester_identity_mismatch") { _ = try rig.receive(message(.acquireRequest, requestFields()), peer: c) }, "requesterId must be the authenticated peer")
    check(throwsCode("requester_identity_mismatch") { _ = try rig.receive(message(.acquireRequest, requestFields()), peerSession: sessB) }, "requesterSessionId must be the peer's session")
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    var wrongGrantor = startFields(from: grant[0]); wrongGrantor["grantorId"] = .string(a)
    check(throwsCode("start_identity_mismatch") { _ = try rig.receive(message(.acquireStart, wrongGrantor)) }, "START grantorId must be ours")
    var wrongSession = startFields(from: grant[0]); wrongSession["grantorSessionId"] = .string(sessA)
    check(throwsCode("start_identity_mismatch") { _ = try rig.receive(message(.acquireStart, wrongSession)) }, "START grantorSessionId must be ours")
    check(throwsCode("cancel_identity_mismatch") { _ = try rig.receive(message(.acquireCancel, ["requestId": .string(id1), "requesterId": .string(c), "requesterSessionId": .string(sessA), "membershipDigest": .string(PeerV2Message.membershipDigest([a, b]))])) }, "CANCEL requester identity")
    check(throwsCode("not_acquisition_message") { _ = try rig.receive(message(.ping, ["nonce": .string(id1)])) }, "non acquisition message")
}

do {  // START rules
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    var unknownGrant = startFields(from: grant[0]); unknownGrant["grantId"] = .string(id1)
    check(denyReason(try! rig.receive(message(.acquireStart, unknownGrant))) == "UNKNOWN", "START with a foreign grant -> EXPIRED -> UNKNOWN on the wire")
    check(rig.coordinator.live?.phase == .preparing, "a foreign START leaves the reservation alone")
    check(denyReason(try! rig.receive(message(.acquireStart, startFields(from: grant[0])), status: status(.held))) == "HELD", "START while holding -> HELD")
    check(rig.coordinator.live == nil && rig.coordinator.reservation?.phase == .cancelled, "failed START cancels the reservation")
}

do {  // START revalidates the revision set
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields(revisions: [a: 7, b: 9])), current: [a: 7, b: 9])
    check(grant[0].kind == .acquireGrant, "grant with revisions")
    check(denyReason(try! rig.receive(message(.acquireStart, startFields(from: grant[0])), current: [a: 7, b: 10])) == "STALE_SNAPSHOT", "START with a changed revision")
    check(rig.coordinator.reservation?.phase == .cancelled, "stale START cancels")
}

do {  // expiry
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    rig.clock.now += 4_999
    check(rig.coordinator.live != nil, "alive at 4.999 s")
    rig.clock.now += 1
    check(rig.coordinator.live == nil, "expired at 5 s (no longer blocks a takeover)")
    check(rig.coordinator.reservation?.phase == .cancelled, "expiry cancels")
    check(denyReason(try! rig.receive(message(.acquireStart, startFields(from: grant[0])))) == "UNKNOWN", "START after expiry -> UNKNOWN")
    check(denyReason(try! rig.receive(message(.acquireRequest, requestFields()))) == "UNKNOWN", "the expired request cannot be replayed")
    check(try! rig.receive(message(.acquireRequest, requestFields(id: "00000000-0000-4000-8000-0000000000e3")))[0].kind == .acquireGrant, "a fresh request is granted after expiry")
    // Lazy expiry (no tick) on a new request.
    rig.clock.now += 5_000
    check(try! rig.receive(message(.acquireRequest, requestFields(id: "00000000-0000-4000-8000-0000000000e4")))[0].kind == .acquireGrant, "request after unnoticed expiry is granted")
}

do {  // CANCEL
    let rig = Rig()
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    let cancel: [String: JSONValue] = ["requestId": grant[0].fields["requestId"]!, "requesterId": .string(a), "requesterSessionId": .string(sessA),
                                       "membershipDigest": grant[0].fields["membershipDigest"]!]
    var wrong = cancel; wrong["requestId"] = .string(id1)
    check(try! rig.receive(message(.acquireCancel, wrong)).isEmpty && rig.coordinator.live != nil, "CANCEL for another request is ignored")
    check(try! rig.receive(message(.acquireCancel, cancel)).isEmpty && rig.coordinator.live == nil, "CANCEL releases the reservation, without a reply")
    // An in-progress reservation is released by the requester's terminal CANCEL as well.
    let second = try! rig.receive(message(.acquireRequest, requestFields(id: "00000000-0000-4000-8000-0000000000e5")))
    _ = try! rig.receive(message(.acquireStart, startFields(from: second[0])))
    var cancel2 = cancel; cancel2["requestId"] = second[0].fields["requestId"]!
    _ = try! rig.receive(message(.acquireCancel, cancel2))
    check(rig.coordinator.live == nil, "terminal CANCEL releases IN_PROGRESS")
}

do {  // local takeover by the user, transitions
    let rig = Rig()
    var seen: [String] = []
    rig.coordinator.onTransition = { _, phase, reason in seen.append("\(phase):\(reason)") }
    let grant = try! rig.receive(message(.acquireRequest, requestFields()))
    check(rig.coordinator.cancelForLocalTakeover(), "user takeover cancels a live reservation")
    check(!rig.coordinator.cancelForLocalTakeover(), "nothing left to cancel")
    check(denyReason(try! rig.receive(message(.acquireStart, startFields(from: grant[0])))) == "UNKNOWN", "requester's START is denied afterwards")
    check(seen == ["preparing:granted", "cancelled:local_takeover_by_user"], "transitions logged: \(seen)")
}

// MARK: Central handshake (PeerLeV2SessionTest)

final class FakeLink: PeerV2SessionHooks {
    var sent: [PeerV2Message.Validated] = []
    var ended: [String] = []
    var ready = false
    var acquisitions: [PeerV2Frame.Kind] = []
    var snapshots: [[String: JSONValue]] = []
    var bound: [String] = []
    var rejectIdentity = false
    func send(_ frame: PeerV2Frame.Frame) -> Bool { sent.append(try! PeerV2Message.decode(frame)); return true }
    func isAuthorized() -> Bool { true }
    func localInstallationId() -> String { b }
    func localSessionId() -> String { sessB }
    func localSnapshot() -> [String: JSONValue] {
        ["sessionId": .string(sessB), "revision": .int(4), "headsetId": .string(headset), "ownership": .string("released"),
         "audioUse": .string("idle"), "lastOwnerId": .null, "lastOwnerRevision": .null, "acquisition": .null]
    }
    func newConnectionId() -> String { id2 }
    func bindPeerIdentity(_ installationId: String) throws { if rejectIdentity { throw V2Error("v2_identity_mismatch") }; bound.append(installationId) }
    func observeSnapshot(remoteSessionId: String, snapshot: [String: JSONValue]) -> Bool { snapshots.append(snapshot); return true }
    func markReady(remoteSessionId: String, acquisitionCapable: Bool) { ready = acquisitionCapable }
    func handleAcquisition(remoteSessionId: String, message: PeerV2Message.Validated) throws { acquisitions.append(message.kind) }
    func onEnded(reason: String) { ended.append(reason) }
}

func record(_ kind: PeerV2Frame.Kind, _ fields: [String: JSONValue]) -> Data { try! PeerV2Message.encode(kind, fields).encode() }
let remoteSnapshot: [String: JSONValue] = ["sessionId": .string(sessA), "revision": .int(2), "headsetId": .null, "ownership": .string("unknown"),
                                           "audioUse": .string("unknown"), "lastOwnerId": .null, "lastOwnerRevision": .null, "acquisition": .null]
let remoteHello: [String: JSONValue] = ["installationId": .string(a), "sessionId": .string(sessA),
    "capabilities": .array([.string("acquire-v1"), .string("snapshot")]), "initiatorId": .string(b), "connectionId": .string(id2)]

do {
    let link = FakeLink(), clock = TestClock()
    let session = PeerV2Session(hooks: link, clock: { clock.now })
    session.start()
    check(link.sent.map(\.kind) == [.hello], "central opens with HELLO")
    let hello = link.sent[0].fields
    check(hello["initiatorId"]?.stringValue == b && hello["installationId"]?.stringValue == b && hello["connectionId"]?.stringValue == id2, "HELLO makes this Mac the initiator")
    check(hello["capabilities"] == .array([.string("acquire-v1"), .string("quiet-v1"), .string("snapshot")]), "HELLO capabilities")
    session.onRecord(record(.hello, remoteHello))
    check(link.bound == [a] && !session.isReady, "peer HELLO binds its identity")
    session.onRecord(record(.snapshot, remoteSnapshot))
    check(link.sent.map(\.kind) == [.hello, .snapshot, .ping], "first peer SNAPSHOT is answered with SNAPSHOT and PING")
    let nonce = link.sent[2].fields["nonce"]!
    session.onRecord(record(.pong, ["nonce": nonce, "snapshot": .object(remoteSnapshot)]))
    check(session.isReady && link.ready, "matching PONG makes the session ready")
    // Once ready, PING is answered with a PONG echoing the nonce and carrying the snapshot (no separate SNAPSHOT).
    let before = link.sent.count
    session.onRecord(record(.ping, ["nonce": .string(id1)]))
    check(link.sent.count == before + 1 && link.sent.last?.kind == .pong && link.sent.last?.fields["nonce"] == .string(id1), "PING -> PONG")
    check(link.sent.last?.fields["snapshot"]?.objectValue?["revision"] == .int(4), "PONG carries the current snapshot")
    // Acquisition messages reach the handler only from an acquire-v1 peer.
    session.onRecord(record(.acquireRequest, requestFields(id: id1)))
    check(link.acquisitions == [.acquireRequest], "acquisition dispatched")
    // Heartbeat: PING every 10 s, closed after two missed intervals.
    clock.now += 10_000; session.tick()
    check(link.sent.last?.kind == .ping, "heartbeat PING after 10 s")
    clock.now += 10_000; session.tick()
    check(link.ended.isEmpty, "one missed interval is tolerated")
    clock.now += 10_000; session.tick()
    check(link.ended == ["heartbeat_timeout"] && session.isEnded, "two missed intervals end the session")
}

do {  // handshake watchdog, wrong identity, missing acquire-v1, one frame per record
    let link = FakeLink(), clock = TestClock()
    let session = PeerV2Session(hooks: link, clock: { clock.now })
    session.start()
    clock.now += 4_999; session.tick()
    check(link.ended.isEmpty, "no timeout before 5 s")
    clock.now += 1; session.tick()
    check(link.ended == ["handshake_timeout"], "5 s handshake watchdog")

    let rejected = FakeLink(); let s2 = PeerV2Session(hooks: rejected, clock: { clock.now })
    rejected.rejectIdentity = true; s2.start(); s2.onRecord(record(.hello, remoteHello))
    check(rejected.ended == ["v2_identity_mismatch"], "identity the LE link did not vouch for ends the session")

    let wrongConnection = FakeLink(); let s3 = PeerV2Session(hooks: wrongConnection, clock: { clock.now })
    s3.start(); s3.onRecord(record(.hello, remoteHello.merging(["connectionId": .string(id1)]) { $1 }))
    check(wrongConnection.ended == ["v2_connection_identity_mismatch"], "HELLO must echo our connectionId")

    let noCap = FakeLink(); let s4 = PeerV2Session(hooks: noCap, clock: { clock.now })
    s4.start(); s4.onRecord(record(.hello, remoteHello.merging(["capabilities": .array([.string("snapshot")])]) { $1 }))
    s4.onRecord(record(.acquireRequest, requestFields(id: id1)))
    check(noCap.ended == ["peer_missing_acquire_v1"], "acquisition from a peer without acquire-v1")

    let early = FakeLink(); let s5 = PeerV2Session(hooks: early, clock: { clock.now })
    s5.start(); s5.onRecord(record(.snapshot, remoteSnapshot))
    check(early.ended == ["hello_required"], "SNAPSHOT before HELLO")

    let two = FakeLink(); let s6 = PeerV2Session(hooks: two, clock: { clock.now })
    s6.start(); s6.onRecord(record(.hello, remoteHello) + record(.snapshot, remoteSnapshot))
    check(two.ended == ["v2_record_not_one_frame"], "a record carries exactly one frame")

    let other = FakeLink(); let s7 = PeerV2Session(hooks: other, clock: { clock.now })
    s7.start(); s7.onRecord(record(.hello, remoteHello))
    s7.onRecord(record(.snapshot, remoteSnapshot.merging(["sessionId": .string(sessB)]) { $1 }))
    check(other.ended == ["v2_session_mismatch"], "snapshot must carry the HELLO session")
}

if failures == 0 { print("PeerV2: all tests passed") } else { print("PeerV2: \(failures) failure(s)"); do {  // quiet-v1 peer: idle probe instead of the 10 s heartbeat
    let link = FakeLink(), clock = TestClock()
    let session = PeerV2Session(hooks: link, clock: { clock.now })
    session.start()
    session.onRecord(record(.hello, remoteHello.merging(["capabilities": .array([.string("acquire-v1"), .string("quiet-v1"), .string("snapshot")])]) { $1 }))
    session.onRecord(record(.snapshot, remoteSnapshot))
    session.onRecord(record(.pong, ["nonce": link.sent.last!.fields["nonce"]!, "snapshot": .object(remoteSnapshot)]))
    check(session.isReady, "quiet peer: ready")
    let afterReady = link.sent.count
    clock.now += 59_000; session.tick()
    check(link.sent.count == afterReady, "quiet peer: no PING in the first minute of silence")
    clock.now += 1_000; session.tick()
    check(link.sent.last?.kind == .ping && link.sent.count == afterReady + 1, "quiet peer: probe after 60 s")
    let probe = link.sent.last!.fields["nonce"]!
    session.onRecord(record(.ping, ["nonce": .string(id1)]))  // the peer probes at the same time
    session.onRecord(record(.pong, ["nonce": probe, "snapshot": .object(remoteSnapshot)]))
    check(link.snapshots.count == 3, "the probe's PONG delivers the snapshot, even after the peer's own PING")
    clock.now += 30_000; session.tick()
    check(link.sent.count == afterReady + 1, "an answered probe restarts the minute")
    clock.now += 30_000; session.tick()
    check(link.sent.count == afterReady + 2, "probe again after another quiet minute")
    clock.now += 15_000; session.tick()
    check(link.sent.count == afterReady + 3 && link.ended.isEmpty, "unanswered: one more probe after 15 s")
    clock.now += 15_000; session.tick()
    check(link.ended == ["heartbeat_timeout"], "two unanswered probes end the session")
}
do {  // quiet-v1 peer: any frame counts as an answer
    let link = FakeLink(), clock = TestClock()
    let session = PeerV2Session(hooks: link, clock: { clock.now })
    session.start()
    session.onRecord(record(.hello, remoteHello.merging(["capabilities": .array([.string("quiet-v1"), .string("snapshot")])]) { $1 }))
    session.onRecord(record(.snapshot, remoteSnapshot))
    session.onRecord(record(.pong, ["nonce": link.sent.last!.fields["nonce"]!, "snapshot": .object(remoteSnapshot)]))
    clock.now += 60_000; session.tick()
    clock.now += 10_000
    session.onRecord(record(.snapshot, remoteSnapshot))
    clock.now += 50_000; session.tick()
    clock.now += 50_000; session.tick()
    check(link.ended.isEmpty, "a SNAPSHOT after the probe keeps the link")
}

exit(1) }
