#!/bin/bash
# Builds and runs the Mac client's unit tests (pure Swift, no Xcode project).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
# Top-level test code must live in a file named main.swift.
cp Tests/PeerGattRecordsTests.swift "$OUT/main.swift"
swiftc -target arm64-apple-macos13.0 Sources/PeerGattRecords.swift "$OUT/main.swift" -o "$OUT/records-tests"
"$OUT/records-tests"
