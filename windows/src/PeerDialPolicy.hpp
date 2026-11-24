#pragma once

// Pure dial/retry rules shared by every peer link on Windows (legacy RFCOMM and
// v2). They mirror the Android client: PeerRetryPolicy (utils/peer/
// PeerConnectionState.kt) and CrossDeviceClient.CONNECT_TIMEOUT_MS. No Windows
// types here so the unit tests can cover them.

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>

namespace Propods {

// Android CrossDeviceClient.CONNECT_TIMEOUT_MS.
inline constexpr std::chrono::milliseconds kPeerConnectTimeout{8000};

// Android PeerRetryPolicy.delaysMs = 2000, 4000, 8000, 15000 (the last repeats);
// jitter factor 0.8..1.2 (CrossDeviceClient: Random.nextDouble(0.8, 1.2000001)).
inline constexpr double kPeerJitterMin = 0.8;
inline constexpr double kPeerJitterMax = 1.2;
// PeerRetryPolicy.MIN_DELAY_DURING_AUDIO_MS: the floor while local audio is active.
inline constexpr std::chrono::milliseconds kPeerMinDelayDuringAudio{15000};

// Delay before dial attempt number attempt+1. `attempt` is the number of failed
// dials since the last successful link (0 after a link came up).
inline std::chrono::milliseconds peerRetryDelay(int attempt, double jitterFactor,
                                                bool localAudioActive = false) {
    static constexpr long long kStepsMs[] = {2000, 4000, 8000, 15000};
    const int idx = std::clamp(attempt, 0, 3);
    long long base = kStepsMs[idx];
    if (localAudioActive) base = std::max(base, kPeerMinDelayDuringAudio.count());
    const double j = std::clamp(jitterFactor, kPeerJitterMin, kPeerJitterMax);
    return std::chrono::milliseconds{static_cast<long long>(static_cast<double>(base) * j)};
}

// Thrown by dialRfcomm() when the peer's Bluetooth device answers but has no
// EveryPods service (EveryPods not running there, or a device that only links
// over Bluetooth LE, such as the Mac).
struct PeerServiceMissing : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Delay after `misses` dials in a row (1 or more) that found no EveryPods
// service: 15 s, 30 s, 1 min, 2 min, then 5 min. Every dial pages the device and
// runs an SDP query; doing that every 15 s while the AirPods were idle on this
// PC made them drop within a minute (2026-11-22: 4,902 dials to the Mac in 10 h).
// The Bluetooth link-up wake does not cut this wait short: our own dials bring
// that link up.
inline std::chrono::milliseconds peerServiceMissingDelay(int misses, double jitterFactor) {
    static constexpr long long kStepsMs[] = {15000, 30000, 60000, 120000, 300000};
    const int idx = std::clamp(misses - 1, 0, 4);
    const double j = std::clamp(jitterFactor, kPeerJitterMin, kPeerJitterMax);
    return std::chrono::milliseconds{static_cast<long long>(static_cast<double>(kStepsMs[idx]) * j)};
}

// A link that ends this soon after it came up counts like a dial that found no
// EveryPods service (peerServiceMissingDelay), so the next dial backs off. A
// peer that accepts and closes at once (e.g. an app version that no longer
// takes this link) otherwise reset the retry count on every connect: the PC
// redialled it every 2-3 s on both services, about 4,000 dials in 90 min
// (2026-11-24), the radio load that made idle AirPods drop before.
inline constexpr std::chrono::milliseconds kPeerShortLink{10000};

inline bool peerLinkTooShort(std::chrono::milliseconds lasted) {
    return lasted < kPeerShortLink;
}

inline double peerRetryJitter() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<double>(kPeerJitterMin, kPeerJitterMax)(rng);
}

}  // namespace Propods
