// TriggerApps: every app has the same three settings; built-ins are pre-configured, new apps start with
// "take when it starts playing", apps seen in a call start with all three.
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}
func d(_ l: TriggerAppList, _ app: String, out: Bool = false, mic: Bool = false, recorder: Bool = false,
       tab: Bool = false) -> AudioUseDecision {
    l.decide(app: app, output: out, input: mic, recorder: recorder, meetingTab: tab)
}

do {
    var l = TriggerAppList()
    check(l.settings("com.example.chat") == .play, "a new app starts with take on play")
    let ding = d(l, "com.example.chat", out: true)
    check(ding.playing && ding.triggers && !ding.trusted && !ding.call, "take on play: playback triggers, filtered")
    check(!d(l, "com.example.chat", out: true, mic: true).call, "a play-only app's mic is not a call")
    check(l.noteCall("com.example.chat") && l.settings("com.example.chat") == .all, "seen in a call: all three")
    let call = d(l, "com.example.chat", out: true, mic: true)
    check(call.call && call.trusted && call.triggers, "then its mic is a call that takes at once")
    check(!l.noteCall("com.example.chat"), "noted once")
}
do {
    let l = TriggerAppList()
    check(l.settings("us.zoom.xos") == .all && l.settings("com.google.Chrome.helper") == .all
          && l.settings("com.apple.Music") == .play && l.settings("com.spotify.client") == .play, "built-in defaults")
    check(l.key("com.microsoft.teams2") == "com.microsoft.teams", "Teams 2 maps to the Teams entry")
    check(l.key("com.cisco.webexmeetingsapp") == "com.cisco.webex", "meeting apps match by prefix")
    check(l.key("com.apple.musicrecognition") != "com.apple.music", "other apps don't match by bare prefix")
    let zoom = d(l, "us.zoom.xos", out: true)
    check(zoom.call && zoom.trusted, "a meeting app's sound alone counts as using the mic")
    let tab = d(l, "com.google.Chrome", out: true, tab: true)
    check(tab.call && tab.trusted, "a browser meeting tab counts as using the mic")
    let yt = d(l, "com.google.Chrome", out: true)
    check(yt.triggers && !yt.trusted && !yt.call, "browser playback is filtered, not a call")
}
do {
    var l = TriggerAppList()
    l.toggle("com.google.Chrome", .takeOnMic)
    l.toggle("com.google.Chrome", .takeOnPlay)
    check(l.settings("com.google.chrome.helper") == TriggerSettings(keepOnMic: true), "built-ins can be changed (keep only)")
    check(l.isChangedBuiltIn("com.google.Chrome"), "changed built-in is flagged")
    let meet = d(l, "com.google.Chrome", out: true, mic: true)
    check(meet.call && !meet.triggers, "keep only: a call that never takes")
    check(!d(l, "com.google.Chrome", out: true).triggers, "keep only: playback never takes")
    check(l.reset("com.google.Chrome") && l.settings("com.google.Chrome") == .all && !l.isChangedBuiltIn("com.google.Chrome"),
          "reset to built-in")
}
do {
    var l = TriggerAppList()
    l.toggle("com.tinyspeck.slackmacgap", .takeOnPlay)
    check(l.settings("com.tinyspeck.slackmacgap") == .none, "unticking the only setting leaves nothing ticked")
    let ding = d(l, "com.tinyspeck.slackmacgap", out: true, mic: true)
    check(ding.playing && !ding.triggers && !ding.call, "nothing ticked: never takes or keeps")
    check(!l.noteCall("com.tinyspeck.slackmacgap"), "a call doesn't override what the user set")
    check(l.settings("com.tinyspeck.slackmacgap.helper") == .none, "a helper follows its app")
}
do {
    let memo = d(TriggerAppList(), "com.apple.VoiceMemos", out: true, mic: true, recorder: true)
    check(!memo.call && !memo.playing && !memo.triggers, "a recorder recording is neither a call nor playback")
}
do {
    let old = TriggerAppList(user: ["a.call": "call", "a.keep": "keep", "a.media": "media", "a.ignore": "ignore", "a.new": "takeonmic,keeponmic"])
    check(old.settings("a.call") == TriggerSettings(takeOnMic: true, keepOnMic: true), "old call → take on mic + keep")
    check(old.settings("a.keep") == TriggerSettings(keepOnMic: true), "old keep → keep")
    check(old.settings("a.media") == .play, "old media → take on play")
    check(old.settings("a.ignore") == .none, "old ignore → nothing ticked")
    check(old.settings("a.new") == TriggerSettings(takeOnMic: true, keepOnMic: true), "flag form loads")
    var l = TriggerAppList(callSeen: ["com.x"])
    l.toggle("com.y", .keepOnMic)
    let back = TriggerAppList(user: l.savedUser, callSeen: Array(l.callSeen))
    check(back.settings("com.y") == l.settings("com.y") && back.settings("com.x") == .all, "saved forms round-trip")
}
exit(failures == 0 ? 0 : 1)
