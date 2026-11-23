// PeerGattRecords.swift
//
// Records carried over the Bluetooth LE peer link (Everypods docs/peer-ble-transport.md §4.2), identical to
// Android's PeerGattRecords.kt: `type (u8) ‖ length (u16, big-endian) ‖ payload`. The characteristics carry a
// byte stream of these; a record may be split across writes or notifications.

import Foundation

enum PeerGattRecords {
    static let hello: UInt8 = 0x01
    static let legacy: UInt8 = 0x02
    static let ping: UInt8 = 0x03
    static let pong: UInt8 = 0x04
    static let v2: UInt8 = 0x05
    /// Setup exchange (authorizing this Mac): central -> phone request, phone -> central answer. Same UTF-8 JSON as the
    /// RFCOMM setup exchange, at most `maxSetupPayload` bytes.
    static let setupRequest: UInt8 = 0x06
    static let setupAck: UInt8 = 0x07
    static let maxSetupPayload = 1024

    static let maxPayload = 4096

    struct Record: Equatable {
        let type: UInt8
        let payload: Data
    }

    struct Hello: Equatable {
        let installationId: String
        let name: String
        let capabilities: [String]
    }

    enum ProtocolError: Error {
        case typeZero, tooLarge, helloInvalid(String)
    }

    static func encode(_ type: UInt8, _ payload: Data) -> Data? {
        guard type != 0, payload.count <= maxPayload else { return nil }
        var out = Data([type, UInt8(payload.count >> 8), UInt8(payload.count & 0xFF)])
        out.append(payload)
        return out
    }

    static func encodeHello(_ hello: Hello) -> Data? {
        let object: [String: Any] = ["v": 1, "id": hello.installationId, "name": hello.name, "caps": hello.capabilities]
        guard let json = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return nil }
        return encode(Self.hello, json)
    }

    static func parseHello(_ payload: Data) throws -> Hello {
        guard let object = (try? JSONSerialization.jsonObject(with: payload)) as? [String: Any] else {
            throw ProtocolError.helloInvalid("not json")
        }
        guard (object["v"] as? Int) == 1 else { throw ProtocolError.helloInvalid("version") }
        guard let id = object["id"] as? String, let uuid = UUID(uuidString: id), uuid.uuidString.lowercased() == id else {
            throw ProtocolError.helloInvalid("id")
        }
        let name = object["name"] as? String ?? ""
        guard name.count <= 248 else { throw ProtocolError.helloInvalid("name") }
        let caps = (object["caps"] as? [Any])?.compactMap { $0 as? String } ?? []
        return Hello(installationId: id, name: name, capabilities: caps)
    }

    /// Splits `data` into chunks of at most `size` bytes (the write length CoreBluetooth allows).
    static func chunk(_ data: Data, size: Int) -> [Data] {
        guard size > 0, !data.isEmpty else { return [] }
        return stride(from: 0, to: data.count, by: size).map {
            data.subdata(in: data.startIndex + $0 ..< data.startIndex + min($0 + size, data.count))
        }
    }

    /// Reassembles records from a byte stream. Unknown types are returned (the caller skips them); a length above
    /// `maxPayload` or a zero type throws, after which the stream cannot be trusted.
    struct Decoder {
        private var pending = Data()

        mutating func feed(_ bytes: Data) throws -> [Record] {
            pending.append(bytes)
            var records: [Record] = []
            var offset = pending.startIndex
            while pending.endIndex - offset >= 3 {
                let type = pending[offset]
                let length = Int(pending[offset + 1]) << 8 | Int(pending[offset + 2])
                if type == 0 { throw ProtocolError.typeZero }
                if length > maxPayload { throw ProtocolError.tooLarge }
                guard pending.endIndex - offset - 3 >= length else { break }
                records.append(Record(type: type, payload: pending.subdata(in: offset + 3 ..< offset + 3 + length)))
                offset += 3 + length
            }
            pending = pending.subdata(in: offset ..< pending.endIndex)
            return records
        }

        mutating func reset() { pending = Data() }
    }
}
