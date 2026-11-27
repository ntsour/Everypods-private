// PeerV2.swift
//
// EveryPods peer protocol v2 codec (Everypods docs/peer-protocol-v2.md): EPH2 frames, strict JSON message
// validation, the membership digest and the headset identity. A port of Android's PeerV2Frame.kt,
// PeerV2Message.kt, PassiveAcquisitionCoordinator.membershipDigest and PeerIdentityStore.headsetIdFromAddress,
// with the same error codes. Pure Swift (no Bluetooth), so Tests/run.sh can build it.

import CryptoKit
import Foundation

struct V2Error: Error, Equatable, CustomStringConvertible {
    let code: String
    init(_ code: String) { self.code = code }
    var description: String { code }
}

/// The JSON values v2 uses. Integers are signed 64-bit (the protocol rejects fractions and exponents).
enum JSONValue: Equatable {
    case null
    case bool(Bool)
    case int(Int64)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    var stringValue: String? { if case .string(let s) = self { return s }; return nil }
    var intValue: Int64? { if case .int(let i) = self { return i }; return nil }
    var arrayValue: [JSONValue]? { if case .array(let a) = self { return a }; return nil }
    var objectValue: [String: JSONValue]? { if case .object(let o) = self { return o }; return nil }
    var isNull: Bool { if case .null = self { return true }; return false }
}

// MARK: Frame

enum PeerV2Frame {
    static let version: UInt8 = 2
    static let headerSize = 10
    static let maxPayloadSize = 4096
    static let maxFrameSize = headerSize + maxPayloadSize
    static let magic: [UInt8] = Array("EPH2".utf8)

    enum Kind: UInt8, CaseIterable {
        case hello = 1, snapshot, ping, pong, acquireRequest, acquireGrant, acquireDeny, acquireCancel,
             relayData, acquireStart, acquireStartAck
    }

    struct Frame: Equatable {
        let kind: Kind
        let payload: Data

        func encode() throws -> Data {
            guard payload.count <= PeerV2Frame.maxPayloadSize else { throw V2Error("oversized_payload") }
            var out = Data(PeerV2Frame.magic)
            out.append(PeerV2Frame.version)
            out.append(kind.rawValue)
            let size = UInt32(payload.count)
            out.append(contentsOf: [UInt8(size >> 24 & 0xFF), UInt8(size >> 16 & 0xFF), UInt8(size >> 8 & 0xFF), UInt8(size & 0xFF)])
            out.append(payload)
            return out
        }
    }

    /// Incremental decoder for fragmented or coalesced reads. Any error leaves the stream unusable.
    struct Decoder {
        private var pending = [UInt8]()

        mutating func feed(_ chunk: Data) throws -> [Frame] {
            pending.append(contentsOf: chunk)
            var frames: [Frame] = []
            while pending.count >= PeerV2Frame.headerSize {
                let size = try Self.parseHeader(pending)
                guard pending.count >= size else { break }
                guard let kind = Kind(rawValue: pending[5]) else { throw V2Error("unknown_type") }
                frames.append(Frame(kind: kind, payload: Data(pending[PeerV2Frame.headerSize ..< size])))
                pending.removeFirst(size)
            }
            guard pending.count <= PeerV2Frame.maxFrameSize else { throw V2Error("decoder_buffer_overflow") }
            return frames
        }

        var bufferedByteCount: Int { pending.count }

        private static func parseHeader(_ bytes: [UInt8]) throws -> Int {
            guard Array(bytes[0 ..< 4]) == PeerV2Frame.magic else { throw V2Error("invalid_magic") }
            guard bytes[4] == PeerV2Frame.version else { throw V2Error("unsupported_version") }
            guard Kind(rawValue: bytes[5]) != nil else { throw V2Error("unknown_type") }
            let size = UInt32(bytes[6]) << 24 | UInt32(bytes[7]) << 16 | UInt32(bytes[8]) << 8 | UInt32(bytes[9])
            guard size <= UInt32(PeerV2Frame.maxPayloadSize) else { throw V2Error("oversized_payload") }
            return PeerV2Frame.headerSize + Int(size)
        }
    }
}

// MARK: Message

/// Strict JSON payload validation. Duplicate keys, unknown fields, fractions and trailing bytes fail closed.
enum PeerV2Message {
    struct Validated {
        let kind: PeerV2Frame.Kind
        let fields: [String: JSONValue]
    }

    /// Sorted. `quiet-v1`: this side pushes every state change, answers PING at once, and probes a quiet link only
    /// after a minute of silence (docs/peer-link-quiet-redesign.md, D1).
    static let capabilities = ["acquire-v1", "quiet-v1", "snapshot"]
    /// Unknown capability names are ignored, not rejected, so a newer peer can advertise one without breaking the link.
    private static let capabilityName = try! NSRegularExpression(pattern: "^[a-z0-9][a-z0-9-]{0,31}$")
    static func validCapabilityName(_ s: String) -> Bool {
        capabilityName.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)) != nil
    }

    static func decode(_ frame: PeerV2Frame.Frame) throws -> Validated {
        guard let json = String(data: frame.payload, encoding: .utf8) else { throw V2Error("invalid_utf8") }
        var parser = JSONParser(Array(json.utf16))
        guard case .object(let fields) = try parser.parse() else { throw V2Error("payload_not_object") }
        try validate(frame.kind, fields)
        return Validated(kind: frame.kind, fields: fields)
    }

    static func encode(_ kind: PeerV2Frame.Kind, _ fields: [String: JSONValue]) throws -> PeerV2Frame.Frame {
        try validate(kind, fields)
        let payload = Data(JSONWriter.write(.object(fields)).utf8)
        guard payload.count <= PeerV2Frame.maxPayloadSize else { throw V2Error("oversized_payload") }
        return PeerV2Frame.Frame(kind: kind, payload: payload)
    }

    /// SHA-256 over the sorted, unique, lowercase UUIDs, each followed by "\n".
    static func membershipDigest(_ members: [String]) -> String {
        let canonical = Set(members.map { $0.lowercased() }).sorted().map { $0 + "\n" }.joined()
        return SHA256.hash(data: Data(canonical.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    /// The headset identity: a UUID built from the first 16 bytes of SHA-256 over the uppercase "AA:BB:..." address
    /// (version nibble 8, RFC variant bits). Accepts the dashed form IOBluetooth uses as well.
    static func headsetId(fromAddress address: String) throws -> String {
        let normalized = address.trimmingCharacters(in: .whitespaces).uppercased().replacingOccurrences(of: "-", with: ":")
        let parts = normalized.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 6, parts.allSatisfy({ $0.count == 2 && $0.allSatisfy(\.isHexDigit) }) else {
            throw V2Error("invalid_bluetooth_address")
        }
        var bytes = Array(SHA256.hash(data: Data(normalized.utf8)).prefix(16))
        bytes[6] = (bytes[6] & 0x0F) | 0x80
        bytes[8] = (bytes[8] & 0x3F) | 0x80
        let h = bytes.map { String(format: "%02x", $0) }.joined()
        let s = Array(h)
        func part(_ a: Int, _ b: Int) -> String { String(s[a ..< b]) }
        return [part(0, 8), part(8, 12), part(12, 16), part(16, 20), part(20, 32)].joined(separator: "-")
    }

    static func isUUID(_ value: String) -> Bool {
        guard let uuid = UUID(uuidString: value) else { return false }
        return uuid.uuidString.lowercased() == value
    }

    // MARK: Validation

    private static let snapshotFields: Set<String> = ["sessionId", "revision", "headsetId", "ownership", "audioUse",
                                                       "lastOwnerId", "lastOwnerRevision", "acquisition"]
    private static let requestFields: Set<String> = ["requestId", "headsetId", "requesterId", "requesterSessionId",
                                                      "membership", "membershipDigest", "snapshotRevisions"]
    private static let grantorFields: Set<String> = ["grantorId", "grantorSessionId"]
    private static let startFields = requestFields.subtracting(["snapshotRevisions"]).union(grantorFields).union(["grantId"])
    private static let ownershipValues: Set<String> = ["unknown", "held", "released"]
    private static let audioUseValues: Set<String> = ["unknown", "idle", "media", "call"]
    private static let denials: Set<String> = ["HELD", "UNKNOWN", "STALE_SNAPSHOT", "MEMBERSHIP_MISMATCH", "BUSY",
                                               "UNSUPPORTED", "SESSION_CHANGED"]
    private static let relayTypes: Set<String> = ["battery", "anc", "ear_state"]

    private static func validate(_ kind: PeerV2Frame.Kind, _ f: [String: JSONValue]) throws {
        switch kind {
        case .hello:
            try exact(f, ["installationId", "sessionId", "capabilities", "initiatorId", "connectionId"])
            for key in ["installationId", "sessionId", "initiatorId", "connectionId"] { try uuid(f, key) }
            guard let caps = f["capabilities"]?.arrayValue else { throw V2Error("invalid_capabilities") }
            let strings = caps.compactMap(\.stringValue)
            guard strings.count == caps.count, strings.allSatisfy(validCapabilityName),
                  strings == Array(Set(strings)).sorted() else { throw V2Error("invalid_capabilities") }
        case .snapshot:
            try snapshot(f)
        case .ping:
            try exact(f, ["nonce"]); try uuid(f, "nonce")
        case .pong:
            try exact(f, ["nonce", "snapshot"]); try uuid(f, "nonce")
            guard let s = f["snapshot"]?.objectValue else { throw V2Error("invalid_snapshot") }
            try snapshot(s)
        case .acquireRequest:
            try exact(f, requestFields); try acquireIdentity(f)
            let members = try membership(f)
            guard let revisions = f["snapshotRevisions"]?.objectValue else { throw V2Error("invalid_snapshot_revisions") }
            guard revisions.keys.allSatisfy(isUUID), revisions.values.allSatisfy({ ($0.intValue ?? -1) >= 0 }),
                  Set(revisions.keys) == Set(members) else { throw V2Error("invalid_snapshot_revisions") }
        case .acquireGrant:
            try exact(f, requestFields.subtracting(["snapshotRevisions"]).union(grantorFields).union(["grantId"]))
            try acquireIdentity(f, grantor: true); try uuid(f, "grantId"); _ = try membership(f)
        case .acquireDeny:
            try exact(f, requestFields.subtracting(["snapshotRevisions"]).union(grantorFields).union(["reason"]))
            try acquireIdentity(f, grantor: true); _ = try membership(f)
            guard let reason = f["reason"]?.stringValue, denials.contains(reason) else { throw V2Error("invalid_deny_reason") }
        case .acquireCancel:
            try exact(f, ["requestId", "requesterId", "requesterSessionId", "membershipDigest"])
            for key in ["requestId", "requesterId", "requesterSessionId"] { try uuid(f, key) }
            try digest(f)
        case .relayData:
            try exact(f, ["subtype", "data"])
            guard let subtype = f["subtype"]?.stringValue, relayTypes.contains(subtype) else { throw V2Error("invalid_relay_subtype") }
            guard let raw = f["data"]?.stringValue, let bytes = Data(base64Encoded: raw),
                  bytes.count <= 2048, bytes.base64EncodedString() == raw else { throw V2Error("invalid_relay_data") }
        case .acquireStart, .acquireStartAck:
            try exact(f, startFields); try acquireIdentity(f, grantor: true); try uuid(f, "grantId")
            _ = try membership(f)
        }
    }

    private static func snapshot(_ f: [String: JSONValue]) throws {
        try exact(f, snapshotFields); try uuid(f, "sessionId")
        guard let revision = f["revision"]?.intValue, revision >= 0 else { throw V2Error("invalid_revision") }
        try nullableUUID(f, "headsetId")
        guard let ownership = f["ownership"]?.stringValue, ownershipValues.contains(ownership) else { throw V2Error("invalid_ownership") }
        guard let audioUse = f["audioUse"]?.stringValue, audioUseValues.contains(audioUse) else { throw V2Error("invalid_audio_use") }
        try nullableUUID(f, "lastOwnerId")
        let ownerRevision = f["lastOwnerRevision"] ?? .null
        let ownerId = f["lastOwnerId"] ?? .null
        guard (ownerId.isNull && ownerRevision.isNull) || (ownerId.stringValue != nil && (ownerRevision.intValue ?? -1) >= 0) else {
            throw V2Error("invalid_last_owner")
        }
        if let a = f["acquisition"], !a.isNull {
            guard let acq = a.objectValue else { throw V2Error("invalid_acquisition") }
            try exact(acq, ["requestId", "requesterId", "phase"]); try uuid(acq, "requestId"); try uuid(acq, "requesterId")
            guard let phase = acq["phase"]?.stringValue, phase == "preparing" || phase == "in_progress" else {
                throw V2Error("invalid_acquisition_phase")
            }
        }
    }

    private static func acquireIdentity(_ f: [String: JSONValue], grantor: Bool = false) throws {
        for key in ["requestId", "headsetId", "requesterId", "requesterSessionId"] { try uuid(f, key) }
        try digest(f)
        if grantor { for key in ["grantorId", "grantorSessionId"] { try uuid(f, key) } }
    }

    private static func membership(_ f: [String: JSONValue]) throws -> [String] {
        guard let ids = f["membership"]?.arrayValue else { throw V2Error("invalid_membership") }
        let values = ids.compactMap(\.stringValue)
        guard values.count == ids.count, values.allSatisfy(isUUID), !values.isEmpty,
              values == Array(Set(values)).sorted() else { throw V2Error("invalid_membership") }
        guard let requester = f["requesterId"]?.stringValue, values.contains(requester) else { throw V2Error("invalid_membership") }
        if let grantor = f["grantorId"], grantor.stringValue.map({ !values.contains($0) }) ?? true {
            throw V2Error("invalid_membership")
        }
        guard f["membershipDigest"]?.stringValue == membershipDigest(values) else { throw V2Error("membership_digest_mismatch") }
        return values
    }

    private static func exact(_ f: [String: JSONValue], _ keys: Set<String>) throws {
        guard Set(f.keys) == keys else { throw V2Error("invalid_fields") }
    }

    private static func uuid(_ f: [String: JSONValue], _ key: String) throws {
        guard let v = f[key]?.stringValue, isUUID(v) else { throw V2Error("invalid_\(key)") }
    }

    private static func nullableUUID(_ f: [String: JSONValue], _ key: String) throws {
        if let v = f[key], !v.isNull { try uuid(f, key) }
    }

    private static func digest(_ f: [String: JSONValue]) throws {
        guard let v = f["membershipDigest"]?.stringValue, v.utf8.count == 64,
              v.utf8.allSatisfy({ ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x61 && $0 <= 0x66) }) else {
            throw V2Error("invalid_membership_digest")
        }
    }
}

// MARK: JSON

private struct JSONParser {
    private let s: [UInt16]
    private var i = 0
    private static let maxDepth = 32

    init(_ units: [UInt16]) { s = units }

    mutating func parse() throws -> JSONValue {
        let v = try value(0)
        ws()
        guard i == s.count else { throw V2Error("trailing_json") }
        return v
    }

    private func ch(_ c: Character) -> UInt16 { c.utf16.first! }

    private mutating func value(_ depth: Int) throws -> JSONValue {
        guard depth <= Self.maxDepth else { throw V2Error("json_too_deep") }
        ws()
        guard i < s.count else { throw V2Error("incomplete_json") }
        switch s[i] {
        case ch("{"): return try object(depth + 1)
        case ch("["): return try array(depth + 1)
        case ch("\""): return .string(try string())
        case ch("t"): try literal("true"); return .bool(true)
        case ch("f"): try literal("false"); return .bool(false)
        case ch("n"): try literal("null"); return .null
        case ch("-"), ch("0") ... ch("9"): return .int(try number())
        default: throw V2Error("invalid_json")
        }
    }

    private mutating func object(_ depth: Int) throws -> JSONValue {
        i += 1; ws()
        var out: [String: JSONValue] = [:]
        if take("}") { return .object(out) }
        while true {
            ws()
            guard i < s.count, s[i] == ch("\"") else { throw V2Error("invalid_json_key") }
            let key = try string()
            guard out[key] == nil else { throw V2Error("duplicate_json_key") }
            ws()
            guard take(":") else { throw V2Error("invalid_json") }
            out[key] = try value(depth)
            ws()
            if take("}") { return .object(out) }
            guard take(",") else { throw V2Error("invalid_json") }
        }
    }

    private mutating func array(_ depth: Int) throws -> JSONValue {
        i += 1; ws()
        var out: [JSONValue] = []
        if take("]") { return .array(out) }
        while true {
            out.append(try value(depth))
            ws()
            if take("]") { return .array(out) }
            guard take(",") else { throw V2Error("invalid_json") }
        }
    }

    private mutating func string() throws -> String {
        guard take("\"") else { throw V2Error("invalid_json_string") }
        var out: [UInt16] = []
        while i < s.count {
            let c = s[i]; i += 1
            if c == ch("\"") { return String(decoding: out, as: UTF16.self) }
            if c == ch("\\") {
                guard i < s.count else { throw V2Error("invalid_escape") }
                let e = s[i]; i += 1
                switch e {
                case ch("\""), ch("\\"), ch("/"): out.append(e)
                case ch("b"): out.append(0x08)
                case ch("f"): out.append(0x0C)
                case ch("n"): out.append(0x0A)
                case ch("r"): out.append(0x0D)
                case ch("t"): out.append(0x09)
                case ch("u"):
                    guard i + 4 <= s.count,
                          let code = UInt16(String(decoding: s[i ..< i + 4], as: UTF16.self), radix: 16) else {
                        throw V2Error("invalid_escape")
                    }
                    out.append(code); i += 4
                default: throw V2Error("invalid_escape")
                }
            } else if c < 0x20 {
                throw V2Error("invalid_control_character")
            } else {
                out.append(c)
            }
        }
        throw V2Error("unterminated_string")
    }

    private mutating func number() throws -> Int64 {
        let start = i
        if s[i] == ch("-") { i += 1 }
        guard i < s.count else { throw V2Error("invalid_number") }
        if s[i] == ch("0") {
            i += 1
        } else {
            guard s[i] >= ch("1"), s[i] <= ch("9") else { throw V2Error("invalid_number") }
            while i < s.count, s[i] >= ch("0"), s[i] <= ch("9") { i += 1 }
        }
        if i < s.count, [ch("."), ch("e"), ch("E"), ch("+")].contains(s[i]) { throw V2Error("integer_required") }
        guard let n = Int64(String(decoding: s[start ..< i], as: UTF16.self)) else { throw V2Error("invalid_number") }
        return n
    }

    private mutating func literal(_ token: String) throws {
        let units = Array(token.utf16)
        guard i + units.count <= s.count, Array(s[i ..< i + units.count]) == units else { throw V2Error("invalid_json") }
        i += units.count
    }

    private mutating func take(_ c: Character) -> Bool {
        if i < s.count, s[i] == ch(c) { i += 1; return true }
        return false
    }

    private mutating func ws() {
        while i < s.count, [0x20, 0x09, 0x0D, 0x0A].contains(s[i]) { i += 1 }
    }
}

private enum JSONWriter {
    /// Compact JSON with keys sorted (the receiver does not depend on key order).
    static func write(_ value: JSONValue) -> String {
        switch value {
        case .null: return "null"
        case .bool(let b): return b ? "true" : "false"
        case .int(let n): return String(n)
        case .string(let s): return quote(s)
        case .array(let a): return "[" + a.map(write).joined(separator: ",") + "]"
        case .object(let o):
            return "{" + o.keys.sorted().map { quote($0) + ":" + write(o[$0]!) }.joined(separator: ",") + "}"
        }
    }

    private static func quote(_ s: String) -> String {
        var out = "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\u{08}": out += "\\b"
            case "\u{0C}": out += "\\f"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if u.value < 0x20 { out += String(format: "\\u%04x", u.value) } else { out.unicodeScalars.append(u) }
            }
        }
        return out + "\""
    }
}
