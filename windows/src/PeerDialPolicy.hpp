#pragma once

// Pure dial/retry rules shared by every peer link on Windows (legacy RFCOMM and
// v2). They mirror the Android client: PeerRetryPolicy (utils/peer/
// PeerConnectionState.kt) and CrossDeviceClient.CONNECT_TIMEOUT_MS. No Windows
// types here so the unit tests can cover them.

#include <algorithm>
#include <chrono>
#include <random>

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

inline double peerRetryJitter() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<double>(kPeerJitterMin, kPeerJitterMax)(rng);
}

}  // namespace Propods
