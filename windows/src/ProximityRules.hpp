#pragma once

// Pure rules for the AirPods "in case" check. No Windows types here so the unit
// tests can cover them. Mirrors macOS ProximityWatcher.swift (cf4d31c).

#include <cstdint>

namespace Propods {

// An out-of-case beacon this recent keeps the AirPods available. Out of their
// case, AirPods advertise every 3-9 s (measured on the Mac 2026-11-23), so this
// rides out gaps and any other AirPods' in-case beacons nearby.
inline constexpr std::int64_t kOutOfCaseWindowMs = 20'000;

// Apple proximity-pairing message (manufacturer data after the company id):
//   [0]=0x07 type, [1]=0x19 length, [2]=0x01, [3-4]=model, [5]=status, [8]=lid.
// Returns false for any other Apple message. Type 0x07 is shared by a shorter
// message (length 0x11, [2]=0x06; seen nearby at a strong signal every 6 s)
// whose byte 8 is 0xff, which reads as "lid closed": accepting it made the
// state flip several times a second. "In case" = lid closed and neither pod in
// an ear; anything ambiguous is NOT in case.
inline bool parseProximityBeacon(const std::uint8_t* data, std::uint32_t len, bool& inCase) {
    if (len < 9 || data[0] != 0x07 || data[1] != 0x19 || data[2] != 0x01) return false;

    const int status = data[5];
    const int lid    = data[8];

    // Ported verbatim from Android BLEManager.parseProximityMessage.
    const bool primaryLeft = ((status >> 5) & 0x01) == 1;
    const bool thisInCase  = ((status >> 6) & 0x01) == 1;
    const bool xorFactor   = primaryLeft ^ thisInCase;
    const bool leftInEar   = xorFactor ? (status & 0x08) != 0 : (status & 0x02) != 0;
    const bool rightInEar  = xorFactor ? (status & 0x02) != 0 : (status & 0x08) != 0;

    const bool lidOpen = ((lid >> 3) & 0x01) == 0;
    inCase = !lidOpen && !leftInEar && !rightInEar;
    return true;
}

// False only when the latest beacon said in case AND no out-of-case beacon
// arrived within kOutOfCaseWindowMs. The in-case reading does not expire: a
// closed case stops advertising soon after it shuts, while opening it or taking
// a pod out starts out-of-case beacons at once. Times are steady-clock ms;
// 0 = never seen.
inline bool airpodsAvailableFor(std::int64_t now, std::int64_t lastInCaseMs, std::int64_t lastOutOfCaseMs) {
    const bool latestSaysInCase = lastInCaseMs > lastOutOfCaseMs;
    const bool recentlyOut = lastOutOfCaseMs != 0 && now - lastOutOfCaseMs < kOutOfCaseWindowMs;
    return !latestSaysInCase || recentlyOut;
}

} // namespace Propods
