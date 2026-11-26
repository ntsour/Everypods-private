// UserIntent: did the user ask for the audio (a click in the playing app, a player's play broadcast)?
import Foundation

var failures = 0
func check(_ ok: Bool, _ name: String) {
    if ok { print("PASS \(name)") } else { print("FAIL \(name)"); failures += 1 }
}

check(UserIntent.owner(processBundle: "com.google.Chrome.helper", parentBundle: nil) == "com.google.Chrome", "Chrome helper → Chrome")
check(UserIntent.owner(processBundle: "com.hnc.Discord.helper.renderer", parentBundle: nil) == "com.hnc.Discord", "Electron renderer → app")
check(UserIntent.owner(processBundle: "com.apple.WebKit.GPU", parentBundle: nil) == "com.apple.Safari", "WebKit → Safari")
check(UserIntent.owner(processBundle: "com.spotify.client", parentBundle: nil) == "com.spotify.client", "an app is its own owner")
check(UserIntent.owner(processBundle: "?", parentBundle: "com.foo.app") == "com.foo.app", "no bundle → parent app")

let chrome = ["com.google.Chrome"]
check(UserIntent.clickAsked(secondsSinceClick: 0.4, frontmost: "com.google.Chrome", owners: chrome), "click in the playing app counts")
check(!UserIntent.clickAsked(secondsSinceClick: 5, frontmost: "com.google.Chrome", owners: chrome), "an old click does not")
check(!UserIntent.clickAsked(secondsSinceClick: 0.4, frontmost: "com.apple.finder", owners: chrome), "a click in another app does not")
check(!UserIntent.clickAsked(secondsSinceClick: 0.4, frontmost: nil, owners: chrome), "no frontmost app: no intent")
check(UserIntent.clickAsked(secondsSinceClick: 1, frontmost: "com.apple.Safari", owners: ["com.apple.Safari"]), "Safari click and WebKit audio")

let seen = Date()
check(UserIntent.playerAsked(events: ["com.spotify.client": seen.addingTimeInterval(0.5)], owners: ["com.spotify.client"], seenAt: seen),
      "Spotify's Playing broadcast right after the audio counts")
check(UserIntent.playerAsked(events: ["com.apple.Music": seen.addingTimeInterval(-1)], owners: ["com.apple.Music"], seenAt: seen),
      "Music's broadcast just before the audio counts")
check(!UserIntent.playerAsked(events: ["com.apple.Music": seen.addingTimeInterval(-30)], owners: ["com.apple.Music"], seenAt: seen),
      "an old broadcast does not")
check(!UserIntent.playerAsked(events: ["com.spotify.client": seen], owners: chrome, seenAt: seen),
      "another player's broadcast does not")
exit(failures == 0 ? 0 : 1)
