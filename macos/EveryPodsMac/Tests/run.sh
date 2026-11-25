#!/bin/bash
# Builds and runs the Mac client's unit tests (pure Swift, no Xcode project).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
# Top-level test code must live in a file named main.swift.
cp Tests/PeerGattRecordsTests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/PeerGattRecords.swift "$OUT/main.swift" -o "$OUT/records-tests"
"$OUT/records-tests"
# Protocol v2: codec, identity, participant decisions, central handshake (no Bluetooth, no Log).
cp Tests/PeerV2Tests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/PeerV2.swift Sources/PeerAcquisition.swift Sources/PeerV2Session.swift "$OUT/main.swift" -o "$OUT/v2-tests"
"$OUT/v2-tests"
# Proximity beacon parsing.
cp Tests/ProximityTests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/ProximityWatcher.swift Sources/Log.swift "$OUT/main.swift" -o "$OUT/proximity-tests"
"$OUT/proximity-tests"
# Lid-open detection (lid bit, re-arm rule).
cp Tests/LidOpenTests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/ProximityWatcher.swift Sources/Log.swift "$OUT/main.swift" -o "$OUT/lid-tests"
"$OUT/lid-tests"
# Resume detection after a release (silence, then sound).
cp Tests/ResumeDetectorTests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/PlaybackLevelProbe.swift Sources/Log.swift "$OUT/main.swift" -o "$OUT/resume-tests"
"$OUT/resume-tests"
