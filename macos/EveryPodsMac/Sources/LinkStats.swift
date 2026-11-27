import Foundation

/// Per-peer link counters, logged once an hour as one STATS line per peer (docs/peer-link-quiet-redesign.md, phase 0).
/// The keys match the Windows and Android lines, so the traces measure idle traffic, connects and link churn directly:
///
///     STATS peer=3F2A9C10 name=Pixel_10 window_s=3600 le_in=1800 le_in_bytes=... legacy_in=... v2_in=...
///     STATS window_s=3600 airpods_state_lookups=3600
///
/// `le` is the LE link itself (connects, ups and downs, transport PINGs, every record); `legacy` and `v2` count the
/// records of each kind it carried.
final class LinkStats {
    static let shared = LinkStats()
    static let reportInterval: TimeInterval = 3600

    enum Channel: String, CaseIterable { case le, legacy, v2 }

    struct Counters: Equatable {
        var framesIn = 0, bytesIn = 0, framesOut = 0, bytesOut = 0
        var dials = 0, dialFailures = 0, up = 0, down = 0, pings = 0
        var lifeMin: TimeInterval?, lifeMax: TimeInterval?, lifeTotal: TimeInterval = 0
    }

    struct Peer {
        var name: String?
        var channels: [Channel: Counters] = [:]
    }

    private let lock = NSLock()
    private var peers: [String: Peer] = [:]
    private var airPodsStateLookups = 0
    private var since: Date

    init(now: Date = Date()) { since = now }

    func named(_ peer: String, _ name: String) { update(peer) { $0.name = name } }
    func frameIn(_ peer: String, _ channel: Channel, bytes: Int) {
        update(peer) { $0.channels[channel, default: Counters()].framesIn += 1; $0.channels[channel, default: Counters()].bytesIn += bytes }
    }
    func frameOut(_ peer: String, _ channel: Channel, bytes: Int) {
        update(peer) { $0.channels[channel, default: Counters()].framesOut += 1; $0.channels[channel, default: Counters()].bytesOut += bytes }
    }
    func dial(_ peer: String, _ channel: Channel) { update(peer) { $0.channels[channel, default: Counters()].dials += 1 } }
    func dialFailed(_ peer: String, _ channel: Channel) {
        update(peer) { $0.channels[channel, default: Counters()].dialFailures += 1 }
    }
    func linkUp(_ peer: String, _ channel: Channel) { update(peer) { $0.channels[channel, default: Counters()].up += 1 } }
    func linkDown(_ peer: String, _ channel: Channel, lifetime: TimeInterval) {
        update(peer) {
            var c = $0.channels[channel, default: Counters()]
            c.down += 1
            c.lifeTotal += lifetime
            c.lifeMin = min(c.lifeMin ?? lifetime, lifetime)
            c.lifeMax = max(c.lifeMax ?? lifetime, lifetime)
            $0.channels[channel] = c
        }
    }
    func ping(_ peer: String, _ channel: Channel) { update(peer) { $0.channels[channel, default: Counters()].pings += 1 } }
    func airPodsStateLookup() { lock.lock(); airPodsStateLookups += 1; lock.unlock() }

    func due(_ now: Date = Date()) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return now.timeIntervalSince(since) >= Self.reportInterval
    }

    /// The lines for the window since the last report, then a fresh window. Idle peers are left out; the local line
    /// is always there.
    func take(_ now: Date = Date()) -> [String] {
        lock.lock(); defer { lock.unlock() }
        let window = Int(now.timeIntervalSince(since))
        var lines = peers.keys.sorted().compactMap { key -> String? in
            let peer = peers[key]!
            guard peer.channels.values.contains(where: { $0 != Counters() }) else { return nil }
            return Self.format(peer: key, name: peer.name, window: window, channels: peer.channels)
        }
        lines.append("STATS window_s=\(window) airpods_state_lookups=\(airPodsStateLookups)")
        peers.removeAll()
        airPodsStateLookups = 0
        since = now
        return lines
    }

    /// One line for one peer. Pure, so it is unit-tested.
    static func format(peer: String, name: String?, window: Int, channels: [Channel: Counters]) -> String {
        var out = "STATS peer=\(token(peer))"
        if let name { out += " name=\(token(name))" }
        out += " window_s=\(window)"
        for channel in Channel.allCases {
            let c = channels[channel] ?? Counters()
            let n = channel.rawValue
            out += " \(n)_in=\(c.framesIn) \(n)_in_bytes=\(c.bytesIn) \(n)_out=\(c.framesOut) \(n)_out_bytes=\(c.bytesOut)"
                + " \(n)_dials=\(c.dials) \(n)_dial_failures=\(c.dialFailures) \(n)_up=\(c.up) \(n)_down=\(c.down)"
                + " \(n)_pings=\(c.pings)"
            if c.down > 0, let lo = c.lifeMin, let hi = c.lifeMax {
                out += " \(n)_life_min_s=\(Int(lo)) \(n)_life_avg_s=\(Int(c.lifeTotal) / c.down) \(n)_life_max_s=\(Int(hi))"
            }
        }
        return out
    }

    /// Spaces and `=` would break key=value parsing.
    static func token(_ s: String) -> String {
        String(s.map { $0 == " " || $0 == "=" ? "_" : $0 })
    }

    private func update(_ peer: String, _ change: (inout Peer) -> Void) {
        lock.lock(); defer { lock.unlock() }
        change(&peers[peer, default: Peer()])
    }
}
