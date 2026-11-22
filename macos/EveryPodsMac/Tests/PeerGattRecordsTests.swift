// PeerGattRecordsTests.swift — run with Tests/run.sh (no Xcode project needed).
// The byte layouts here are the same fixtures as Android's PeerGattRecordsTest.kt.

import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ message: String, line: Int = #line) {
    if !condition() { failures += 1; print("FAIL line \(line): \(message)") }
}

let status = Data([0x00, 0x01, 0x00, 0x01])

// Encoding: type, big-endian length, payload.
check(PeerGattRecords.encode(PeerGattRecords.legacy, status) == Data([0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01]), "legacy record bytes")
let big = PeerGattRecords.encode(PeerGattRecords.v2, Data(count: 300))!
check(big[1] == 0x01 && big[2] == 0x2C && big.count == 303, "big-endian length")
check(PeerGattRecords.encode(PeerGattRecords.v2, Data(count: 4097)) == nil, "oversized payload refused")

// Decoding across every split point.
let stream = PeerGattRecords.encode(PeerGattRecords.legacy, status)! +
    PeerGattRecords.encode(PeerGattRecords.ping, Data([0, 0, 0, 7]))! +
    PeerGattRecords.encode(PeerGattRecords.v2, Data())!
for split in 1 ..< stream.count {
    var decoder = PeerGattRecords.Decoder()
    let records = (try! decoder.feed(stream.prefix(split))) + (try! decoder.feed(stream.suffix(from: split)))
    check(records.count == 3, "split \(split): count")
    check(records.first == PeerGattRecords.Record(type: PeerGattRecords.legacy, payload: status), "split \(split): legacy")
    check(records.count > 1 && records[1].payload == Data([0, 0, 0, 7]), "split \(split): ping")
}

// MTU-sized chunks of a large record.
let large = PeerGattRecords.encode(PeerGattRecords.v2, Data((0 ..< 1000).map { UInt8($0 & 0xFF) }))!
let chunks = PeerGattRecords.chunk(large, size: 20)
check(chunks.count == 51, "chunk count")
var chunkDecoder = PeerGattRecords.Decoder()
let reassembled = chunks.flatMap { try! chunkDecoder.feed($0) }
check(reassembled.count == 1 && reassembled[0].payload.count == 1000 && reassembled[0].payload[999] == UInt8(999 & 0xFF), "reassembly")

// Unknown types pass through; bad headers throw.
var unknown = PeerGattRecords.Decoder()
check((try! unknown.feed(PeerGattRecords.encode(0x7F, Data([1, 2]))!)).first?.type == 0x7F, "unknown type returned")
var tooLarge = PeerGattRecords.Decoder()
check((try? tooLarge.feed(Data([0x02, 0x10, 0x01]))) == nil, "length above limit throws")
var zero = PeerGattRecords.Decoder()
check((try? zero.feed(Data([0x00, 0x00, 0x00]))) == nil, "type zero throws")

// HELLO round trip, including one produced by Android's org.json (field order differs).
let hello = PeerGattRecords.Hello(installationId: "0b7b2c43-5a0f-4c1e-9d2b-1f6f2d3c4b5a", name: "MacBook Air", capabilities: ["legacy1"])
var helloDecoder = PeerGattRecords.Decoder()
let helloRecord = try! helloDecoder.feed(PeerGattRecords.encodeHello(hello)!).first!
check(helloRecord.type == PeerGattRecords.hello && (try? PeerGattRecords.parseHello(helloRecord.payload)) == hello, "hello round trip")
let android = #"{"v":1,"id":"0b7b2c43-5a0f-4c1e-9d2b-1f6f2d3c4b5a","name":"Pixel 10","caps":["legacy1"]}"#
check((try? PeerGattRecords.parseHello(Data(android.utf8)))?.name == "Pixel 10", "android hello parses")
check((try? PeerGattRecords.parseHello(Data("not json".utf8))) == nil, "bad hello rejected")
check((try? PeerGattRecords.parseHello(Data(#"{"v":2,"id":"0b7b2c43-5a0f-4c1e-9d2b-1f6f2d3c4b5a"}"#.utf8))) == nil, "wrong version rejected")
check((try? PeerGattRecords.parseHello(Data(#"{"v":1,"id":"0B7B2C43-5A0F-4C1E-9D2B-1F6F2D3C4B5A"}"#.utf8))) == nil, "non-canonical id rejected")

if failures == 0 { print("PeerGattRecords: all tests passed") } else { print("PeerGattRecords: \(failures) failure(s)"); exit(1) }
