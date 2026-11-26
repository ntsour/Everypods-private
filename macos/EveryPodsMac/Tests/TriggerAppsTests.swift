// TriggerApps: per-app rules for what takes or keeps the AirPods (mirrors windows/src/TriggerApps.hpp).
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}

do {
    let list = TriggerAppList()
    let ding = list.decide(app: "com.google.Chrome", output: true, input: false, recorder: false)
    check(ding.playing && ding.triggers && !ding.trusted && !ding.call, "unlisted output: playback that must pass the checks")
    let meet = list.decide(app: "com.google.Chrome", output: true, input: true, recorder: false)
    check(meet.call && meet.triggers, "unlisted mic + output: a call")
    let memo = list.decide(app: "com.apple.VoiceMemos", output: true, input: true, recorder: true)
    check(!memo.call && !memo.playing, "a recorder recording is neither a call nor playback")
    let zoom = list.decide(app: "us.zoom.xos", output: true, input: false, recorder: false)
    check(zoom.call && zoom.triggers, "built-in meeting app: output alone is a call")
    check(list.find("com.microsoft.teams2")?.builtIn == true, "built-ins match by prefix")
}
do {
    var list = TriggerAppList()
    check(list.add("com.google.Chrome", .keep), "add keep")
    let meet = list.decide(app: "com.google.chrome", output: true, input: true, recorder: false)
    check(meet.call && !meet.triggers, "keep: mic is a call but never takes")
    let ding = list.decide(app: "com.google.Chrome", output: true, input: false, recorder: false)
    check(ding.playing && !ding.triggers && !ding.call, "keep: playback never takes")
}
do {
    var list = TriggerAppList()
    list.add("com.tinyspeck.slackmacgap", .ignore)
    let ding = list.decide(app: "com.tinyspeck.slackmacgap", output: true, input: false, recorder: false)
    check(ding.playing && !ding.triggers, "ignore: never takes, still counts as playing")
    let huddle = list.decide(app: "com.tinyspeck.slackmacgap", output: true, input: true, recorder: false)
    check(!huddle.call && !huddle.triggers, "ignore: its mic is not a call either")
}
do {
    var list = TriggerAppList()
    list.add("com.spotify.client", .media)
    let play = list.decide(app: "com.spotify.client", output: true, input: false, recorder: false)
    check(play.triggers && play.trusted, "media: playback takes without checks")
    check(list.find("com.spotify.client.helper")?.kind == .media, "a sub-bundle follows its app's rule")
}
do {
    var list = TriggerAppList()
    list.add("ai.dots", .call)
    check(list.decide(app: "ai.dots", output: false, input: true, recorder: false).call, "user call app: mic alone is a call")
    let ping = list.decide(app: "ai.dots", output: true, input: false, recorder: false)
    check(!ping.call && ping.triggers && !ping.trusted, "user call app: output alone is ordinary playback")
}
do {
    var list = TriggerAppList()
    check(!list.add("us.zoom.xos", .ignore), "built-ins can't be overridden")
    check(!list.add("  ", .media), "empty names are refused")
    check(list.add("Com.Foo.App", .media) && !list.add("com.foo.app", .media), "names are normalized; no-op add is false")
    check(list.add("com.foo.app", .ignore) && list.user.count == 1, "re-adding changes the kind")
    let restored = TriggerAppList(user: list.saved)
    check(restored.user == list.user, "saved form round-trips")
    check(TriggerAppList(user: ["com.x": "bogus"]).user.isEmpty, "unknown kinds are dropped")
    check(list.remove("COM.FOO.APP") && list.user.isEmpty, "remove")
}
exit(failures == 0 ? 0 : 1)
