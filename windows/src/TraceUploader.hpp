#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Propods {

// Sends this PC's recent trace files (see Logger.hpp) to a peer's EveryPods
// trace-upload RFCOMM service (its own service, not a v2 frame). A peer that does
// not offer it, or has "Full handover trace" off, reports an error and nothing
// is sent. Android stores uploads next to its own trace so `adb pull` collects both. Blocking; run it off the UI thread.
//
// Only files that are new or have grown since the last successful upload to
// that peer are sent (sizes remembered in traces\sent-<address>.txt).
//
// Wire format (big-endian u32), per file: nameLength, name, size, bytes; the
// peer answers one byte, 1 = stored, 0 = rejected. nameLength 0 ends.
struct TraceUploadResult {
    int stored = 0;
    int rejected = 0;
    int skipped = 0;  // already on the peer at the same size
    std::string error;  // empty on success
};

TraceUploadResult uploadTraces(std::uint64_t peerAddress, std::chrono::hours maxAge);

}
