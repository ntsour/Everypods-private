#pragma once

// Per-peer link counters, logged once an hour as one STATS line per peer
// (docs/peer-link-quiet-redesign.md, phase 0). The keys are the same on every
// client, so the traces measure idle traffic, dials and link churn directly.
//
//   STATS peer=30E04486612C window_s=3600 legacy_in=12 legacy_in_bytes=48 ...
//   STATS window_s=3600 airpods_state_lookups=5400
//
// Header-only and free of WinRT so peer_v2_tests can cover the formatting.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Propods::stats {

enum class Channel : int { Legacy = 0, V2 = 1 };

struct ChannelCounters {
    std::uint64_t framesIn = 0, bytesIn = 0;
    std::uint64_t framesOut = 0, bytesOut = 0;
    std::uint64_t dials = 0, dialFailures = 0;
    std::uint64_t linksUp = 0, linksDown = 0;
    std::uint64_t pings = 0;            // liveness probes we sent
    std::int64_t lifeMinMs = -1;        // links that ended in the window
    std::int64_t lifeMaxMs = -1;
    std::int64_t lifeTotalMs = 0;

    bool empty() const {
        return framesIn == 0 && framesOut == 0 && dials == 0 && linksUp == 0 && linksDown == 0 && pings == 0;
    }
};

struct PeerCounters {
    ChannelCounters channel[2];
    ChannelCounters& operator[](Channel c) { return channel[static_cast<int>(c)]; }
    const ChannelCounters& operator[](Channel c) const { return channel[static_cast<int>(c)]; }
};

inline void appendChannel(std::string& out, const char* name, const ChannelCounters& c) {
    out += std::format(" {0}_in={1} {0}_in_bytes={2} {0}_out={3} {0}_out_bytes={4} {0}_dials={5}"
                       " {0}_dial_failures={6} {0}_up={7} {0}_down={8} {0}_pings={9}",
                       name, c.framesIn, c.bytesIn, c.framesOut, c.bytesOut, c.dials,
                       c.dialFailures, c.linksUp, c.linksDown, c.pings);
    if (c.linksDown > 0) {
        out += std::format(" {0}_life_min_s={1} {0}_life_avg_s={2} {0}_life_max_s={3}", name,
                           c.lifeMinMs / 1000,
                           c.lifeTotalMs / 1000 / static_cast<std::int64_t>(c.linksDown),
                           c.lifeMaxMs / 1000);
    }
}

// One line for one peer. Pure, so it is unit-tested.
inline std::string formatPeerLine(std::uint64_t peer, std::int64_t windowSeconds, const PeerCounters& c) {
    std::string out = std::format("STATS peer={:012X} window_s={}", peer, windowSeconds);
    appendChannel(out, "legacy", c[Channel::Legacy]);
    appendChannel(out, "v2", c[Channel::V2]);
    return out;
}

inline std::string formatLocalLine(std::int64_t windowSeconds, std::uint64_t airpodsStateLookups) {
    return std::format("STATS window_s={} airpods_state_lookups={}", windowSeconds, airpodsStateLookups);
}

class LinkStats {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kReportInterval = std::chrono::hours{1};

    static LinkStats& instance() {
        static LinkStats s;
        return s;
    }

    void frameIn(std::uint64_t peer, Channel ch, std::size_t bytes) {
        std::scoped_lock lk{m_mutex};
        auto& c = m_peers[peer][ch];
        ++c.framesIn;
        c.bytesIn += bytes;
    }
    void frameOut(std::uint64_t peer, Channel ch, std::size_t bytes) {
        std::scoped_lock lk{m_mutex};
        auto& c = m_peers[peer][ch];
        ++c.framesOut;
        c.bytesOut += bytes;
    }
    void dial(std::uint64_t peer, Channel ch, bool ok) {
        std::scoped_lock lk{m_mutex};
        auto& c = m_peers[peer][ch];
        ++c.dials;
        if (!ok) ++c.dialFailures;
    }
    void linkUp(std::uint64_t peer, Channel ch) {
        std::scoped_lock lk{m_mutex};
        ++m_peers[peer][ch].linksUp;
    }
    void linkDown(std::uint64_t peer, Channel ch, std::chrono::milliseconds lifetime) {
        std::scoped_lock lk{m_mutex};
        auto& c = m_peers[peer][ch];
        const std::int64_t ms = lifetime.count();
        ++c.linksDown;
        c.lifeTotalMs += ms;
        if (c.lifeMinMs < 0 || ms < c.lifeMinMs) c.lifeMinMs = ms;
        if (ms > c.lifeMaxMs) c.lifeMaxMs = ms;
    }
    void ping(std::uint64_t peer, Channel ch) {
        std::scoped_lock lk{m_mutex};
        ++m_peers[peer][ch].pings;
    }
    void airpodsStateLookup() {
        std::scoped_lock lk{m_mutex};
        ++m_airpodsStateLookups;
    }

    // True once kReportInterval has passed since the last report.
    bool due(Clock::time_point now) const {
        std::scoped_lock lk{m_mutex};
        return now - m_since >= kReportInterval;
    }

    // The lines for the window since the last report, then a fresh window.
    // Peers with no activity are left out; the local line is always there.
    std::vector<std::string> take(Clock::time_point now) {
        std::scoped_lock lk{m_mutex};
        const auto window = std::chrono::duration_cast<std::chrono::seconds>(now - m_since).count();
        std::vector<std::string> lines;
        for (const auto& [peer, c] : m_peers)
            if (!c[Channel::Legacy].empty() || !c[Channel::V2].empty())
                lines.push_back(formatPeerLine(peer, window, c));
        lines.push_back(formatLocalLine(window, m_airpodsStateLookups));
        m_peers.clear();
        m_airpodsStateLookups = 0;
        m_since = now;
        return lines;
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::uint64_t, PeerCounters> m_peers;
    std::uint64_t m_airpodsStateLookups = 0;
    Clock::time_point m_since = Clock::now();
};

}  // namespace Propods::stats
