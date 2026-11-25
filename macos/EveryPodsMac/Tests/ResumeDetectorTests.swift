// ResumeDetector: silence then sustained sound fires once; sound without prior silence never fires.
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}
/// Feeds `peak` every 10 ms from `from` to `to` seconds; returns how many times it fired.
func run(_ d: inout ResumeDetector, _ peak: Float, _ from: Double, _ to: Double) -> Int {
    var fired = 0
    var t = from
    while t < to { if d.feed(peak: peak, at: t) { fired += 1 }; t += 0.01 }
    return fired
}

do {
    var d = ResumeDetector()
    check(run(&d, 0.2, 0, 1) == 0, "loud before any silence does not fire (pause did not take)")
    check(run(&d, 0, 1, 2) == 0, "silence alone does not fire")
    check(run(&d, 0.2, 2, 3) == 1, "silence then sound fires once")
    check(run(&d, 0, 3, 4) + run(&d, 0.2, 4, 5) == 0, "never fires twice")
}
do {
    var d = ResumeDetector()
    _ = run(&d, 0, 0, 0.3)
    check(run(&d, 0.2, 0.3, 1) == 0, "too little silence does not arm")
}
do {
    var d = ResumeDetector()
    _ = run(&d, 0, 0, 1)
    check(run(&d, 0.2, 1, 1.1) == 0, "a 100 ms blip does not fire")
    _ = run(&d, 0, 1.1, 1.2)
    check(run(&d, 0.2, 1.2, 1.6) == 1, "sustained sound after a blip fires")
}
do {
    var d = ResumeDetector()
    _ = run(&d, 0, 0, 1)
    check(run(&d, 0.001, 1, 3) == 0, "quiet hiss between thresholds does not fire")
}
exit(failures == 0 ? 0 : 1)
