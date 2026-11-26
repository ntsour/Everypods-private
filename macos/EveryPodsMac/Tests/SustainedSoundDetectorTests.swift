// SustainedSoundDetector: the media-start trigger needs about 1 s of real sound, not just an open stream.
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}
/// Feeds `peak` every 10 ms from `from` to `to` seconds; returns the time it fired, if it did.
@discardableResult
func run(_ d: inout SustainedSoundDetector, _ peak: Float, _ from: Double, _ to: Double) -> Double? {
    var t = from
    var firedAt: Double?
    while t < to - 1e-9 {
        if d.feed(peak: peak, at: t), firedAt == nil { firedAt = t }
        t += 0.01
    }
    return firedAt
}

do {
    var d = SustainedSoundDetector()
    check(run(&d, 0, 0, 15) == nil, "an open stream of digital silence never fires")
}
do {
    // A notification ding: half a second of sound, then the browser keeps the stream open, silent.
    var d = SustainedSoundDetector()
    check(run(&d, 0.3, 0, 0.5) == nil && run(&d, 0, 0.5, 15) == nil, "a 0.5 s ding then silence does not fire")
}
do {
    var d = SustainedSoundDetector()
    check(run(&d, 0.3, 0, 0.9) == nil && run(&d, 0, 0.9, 5) == nil, "a 0.9 s chime does not fire")
}
do {
    // Two dings a second apart: the gap starts the count over.
    var d = SustainedSoundDetector()
    run(&d, 0.3, 0, 0.6); run(&d, 0, 0.6, 1.6)
    check(run(&d, 0.3, 1.6, 2.2) == nil && run(&d, 0, 2.2, 5) == nil, "two separate dings do not add up")
}
do {
    var d = SustainedSoundDetector()
    let at = run(&d, 0.3, 0, 3)
    check(at != nil && at! >= 0.99 && at! <= 1.05, "music fires after about 1 s")
    check(run(&d, 0.3, 3, 5) == nil, "fires only once")
}
do {
    // Speech: 0.4 s of words, 0.2 s pauses (below the threshold).
    var d = SustainedSoundDetector()
    var fired: Double?
    var t = 0.0
    while t < 4, fired == nil {
        fired = run(&d, 0.1, t, t + 0.4) ?? run(&d, 0, t + 0.4, t + 0.6)
        t += 0.6
    }
    check(fired != nil && fired! < 2, "speech with short pauses fires within 2 s")
}
do {
    var d = SustainedSoundDetector()
    check(run(&d, 0.001, 0, 5) == nil, "quiet hiss below the loud threshold does not fire")
}
do {
    // Music started a while after a ding, on the same still-open stream.
    var d = SustainedSoundDetector()
    run(&d, 0.3, 0, 0.5); run(&d, 0, 0.5, 8)
    let at = run(&d, 0.3, 8, 10)
    check(at != nil && at! < 9.1, "music later on the same stream still fires")
}
do {
    // Sparse buffers (a stall) do not count as long sound.
    var d = SustainedSoundDetector()
    check(!d.feed(peak: 0.3, at: 0) && !d.feed(peak: 0.3, at: 0.3) && !d.feed(peak: 0.3, at: 0.6),
          "a stall between buffers is not counted as sound")
}
exit(failures == 0 ? 0 : 1)
