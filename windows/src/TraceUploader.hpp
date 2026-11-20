#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Propods {

// Sends this PC's recent trace files (see Logger.hpp) to an Android peer's
// EveryPods trace-upload RFCOMM service. The phone accepts uploads only while
// "Full handover trace" is on in its Debug screen, and stores them next to its
// own trace so `adb pull` collects both. Blocking; run it off the UI thread.
//
// Wire format (big-endian u32), per file: nameLength, name, size, bytes; the
// phone answers one byte, 1 = stored, 0 = rejected. nameLength 0 ends.
struct TraceUploadResult {
    int stored = 0;
    int rejected = 0;
    std::string error;  // empty on success
};

TraceUploadResult uploadTraces(std::uint64_t peerAddress, std::chrono::hours maxAge);

}
