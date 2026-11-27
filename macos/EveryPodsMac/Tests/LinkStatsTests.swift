// LinkStats: the hourly STATS lines (docs/peer-link-quiet-redesign.md, phase 0).
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}

do {
    let line = LinkStats.format(peer: "3F2A9C10", name: "Pixel 10", window: 3600, channels: [:])
    check(line.hasPrefix("STATS peer=3F2A9C10 name=Pixel_10 window_s=3600 le_in=0 le_in_bytes=0"), "names lose spaces")
    check(line.contains(" legacy_in=0 ") && line.contains(" v2_pings=0") && !line.contains("life"),
          "every channel is listed, no lifetimes without a closed link")
}
do {
    let t0 = Date(timeIntervalSince1970: 1_000_000)
    let s = LinkStats(now: t0)
    check(!s.due(t0), "not due at the start")
    s.named("A", "pj")
    s.dial("A", .le)
    s.dialFailed("A", .le)
    s.dial("A", .le)
    s.linkUp("A", .le)
    s.frameOut("A", .le, bytes: 7)
    s.frameOut("A", .v2, bytes: 7)
    s.frameIn("A", .legacy, bytes: 4)
    s.ping("A", .le)
    s.linkDown("A", .le, lifetime: 45)
    s.linkDown("A", .le, lifetime: 15)
    s.airPodsStateLookup()
    check(s.due(t0.addingTimeInterval(3600)), "due after an hour")
    let lines = s.take(t0.addingTimeInterval(3600))
    check(lines.count == 2, "one peer line and the local line")
    check(lines[0].contains(" le_dials=2 le_dial_failures=1 le_up=1 le_down=2 le_pings=1"), "LE link counters")
    check(lines[0].contains(" le_life_min_s=15 le_life_avg_s=30 le_life_max_s=45"), "lifetimes")
    check(lines[0].contains(" v2_out=1 v2_out_bytes=7 ") && lines[0].contains(" legacy_in=1 legacy_in_bytes=4 "),
          "record counts per kind")
    check(lines[1] == "STATS window_s=3600 airpods_state_lookups=1", "local line")
    let next = s.take(t0.addingTimeInterval(7200))
    check(next == ["STATS window_s=3600 airpods_state_lookups=0"], "a fresh window leaves idle peers out")
}

exit(failures == 0 ? 0 : 1)
