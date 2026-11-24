// ProximityTests.swift — run with Tests/run.sh. Beacon bytes as captured on 2026-11-23 (after the company id).

import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ message: String, line: Int = #line) {
    if !condition() { failures += 1; print("FAIL line \(line): \(message)") }
}

// AirPods proximity pairing, both pods in ears, lid open.
let inEars: [UInt8] = [0x07, 0x19, 0x01, 0x27, 0x20, 0x0b, 0x99, 0x8f, 0x11, 0x00]
check(ProximityWatcher.parse(inEars) == false, "pods in ears: out of case")

// Same message, pods in the case (status 0x51: this pod in case, none in an ear), lid closed (bit 3 set).
let stowed: [UInt8] = [0x07, 0x19, 0x01, 0x27, 0x20, 0x51, 0x99, 0x8f, 0x08, 0x00]
check(ProximityWatcher.parse(stowed) == true, "closed case: in case")
var opened = stowed; opened[8] = 0x00
check(ProximityWatcher.parse(opened) == false, "open lid: out of case")

// The shorter type-0x07 message seen nearby: byte 8 is 0xff ("lid closed") but it is not a pairing beacon.
let short: [UInt8] = [0x07, 0x11, 0x06, 0x29, 0x20, 0x0a, 0x51, 0xff, 0xff, 0x5a]
check(ProximityWatcher.parse(short) == nil, "short 0x07 message ignored")
check(ProximityWatcher.parse([0x10, 0x05, 0x01]) == nil, "other Apple message ignored")
check(ProximityWatcher.parse(Array(inEars.prefix(8))) == nil, "truncated ignored")

if failures > 0 { print("Proximity: \(failures) failure(s)"); exit(1) }
print("Proximity: all tests passed")
