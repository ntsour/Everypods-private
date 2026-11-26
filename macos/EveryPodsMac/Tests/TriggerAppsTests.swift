// TriggerApps: three independent per-app settings (take on mic, take on play, keep on mic), mirroring Windows.
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}
let micOnly = (output: false, input: true), playOnly = (output: true, input: false), both = (output: true, input: true)
func d(_ list: TriggerAppList, _ app: String, _ io: (output: Bool, input: Bool), recorder: Bool = false) -> AudioUseDecision {
    list.decide(app: app, output: io.output, input: io.input, recorder: recorder)
}

do {
    let list = TriggerAppList()
    let ding = d(list, "com.google.Chrome", playOnly)
    check(ding.playing && ding.triggers && !ding.trusted && !ding.call, "unlisted output: playback that must pass the checks")
    let meet = d(list, "com.google.Chrome", both)
    check(meet.call && meet.callTrigger && meet.triggers, "unlisted mic + output: a call")
    let memo = d(list, "com.apple.VoiceMemos", both, recorder: true)
    check(!memo.call && !memo.playing, "a recorder recording is neither a call nor playback")
    let zoom = d(list, "us.zoom.xos", playOnly)
    check(zoom.call && zoom.triggers, "built-in meeting app: output alone is a call")
    check(list.find("com.microsoft.teams2")?.builtIn == true, "built-ins match by prefix")
}
do {
    var list = TriggerAppList()
    list.set("com.google.Chrome", TriggerSettings(keepOnMic: true))
    let meet = d(list, "com.google.chrome", both)
    check(meet.call && !meet.triggers, "keep only: mic is a call but never takes")
    let ding = d(list, "com.google.Chrome", playOnly)
    check(ding.playing && !ding.triggers && !ding.call, "keep only: playback never takes")
}
do {
    var list = TriggerAppList()
    list.set("ai.dots", TriggerSettings(takeOnMic: true))
    let mic = d(list, "ai.dots", micOnly)
    check(mic.triggers && mic.callTrigger && mic.trusted && !mic.call, "take on mic only: takes, but is not reported as a call")
    check(!d(list, "ai.dots", playOnly).triggers, "take on mic only: playback never takes")
    list.set("ai.dots", TriggerSettings(takeOnMic: true, keepOnMic: true))
    let call = d(list, "ai.dots", micOnly)
    check(call.triggers && call.call, "take on mic + keep: a call that takes and keeps")
}
do {
    var list = TriggerAppList()
    list.set("com.spotify.client", TriggerSettings(takeOnPlay: true))
    let play = d(list, "com.spotify.client", playOnly)
    check(play.triggers && play.trusted && !play.callTrigger, "take on play: playback takes without checks")
    check(list.find("com.spotify.client.helper")?.settings.takeOnPlay == true, "a sub-bundle follows its app")
    check(!d(list, "com.spotify.client", micOnly).triggers, "take on play: the mic does nothing")
}
do {
    var list = TriggerAppList()
    list.set("com.tinyspeck.slackmacgap", .never)
    let ding = d(list, "com.tinyspeck.slackmacgap", playOnly)
    check(ding.playing && !ding.triggers, "never: still counts as playing, never takes")
    let huddle = d(list, "com.tinyspeck.slackmacgap", both)
    check(!huddle.call && !huddle.triggers, "never: its mic is not a call either")
}
do {
    var list = TriggerAppList()
    check(list.toggle("com.foo.app", .takeOnPlay) && list.user.count == 1, "ticking a setting adds the app")
    check(list.toggle("com.foo.app", .keepOnMic) && list.find("com.foo.app")?.settings == TriggerSettings(takeOnPlay: true, keepOnMic: true),
          "settings combine")
    list.toggle("com.foo.app", .takeOnPlay)
    check(list.toggle("com.foo.app", .keepOnMic) && list.user.isEmpty, "unticking all three removes the app")
    check(!list.toggle("us.zoom.xos", .takeOnPlay), "built-ins can't be changed")
}
do {
    let old = TriggerAppList(user: ["a.call": "call", "a.keep": "keep", "a.media": "media", "a.ignore": "ignore"])
    check(old.find("a.call")?.settings == TriggerSettings(takeOnMic: true, keepOnMic: true), "old call → take on mic + keep")
    check(old.find("a.keep")?.settings == TriggerSettings(keepOnMic: true), "old keep → keep")
    check(old.find("a.media")?.settings == TriggerSettings(takeOnPlay: true), "old media → take on play")
    check(old.find("a.ignore")?.settings.isNever == true, "old ignore → never")
    var list = TriggerAppList()
    list.set("Com.Foo.App", TriggerSettings(takeOnMic: true, takeOnPlay: true))
    list.set("com.bar", .never)
    check(TriggerAppList(user: list.saved).user.sorted { $0.bundle < $1.bundle } == list.user.sorted { $0.bundle < $1.bundle },
          "saved form round-trips (names normalized, never kept)")
    check(!list.set("  ", .never), "empty names are refused")
}
exit(failures == 0 ? 0 : 1)
