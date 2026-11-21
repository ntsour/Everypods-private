# EveryPods for Mac

A menu-bar app that makes your AirPods follow you between a Mac and an Android phone running EveryPods.
It is the macOS counterpart of the Windows tray client (`../../windows`): same cross-device protocol,
same idea ("AirPods follow attention"), built on IOBluetooth and CoreAudio.

## What it does
- Keeps a Bluetooth link to the Android app and answers its status pings.
- **Phone asks for the AirPods** (it started audio or a call): pauses what is playing on the Mac, then releases them.
- **Mac starts playing audio**: asks the phone to release them and connects them here (can be turned off in the menu).
- Tells the phone when a microphone app is active on the Mac so the Android app can hold back takeovers during a call.
- A 10 second cooldown after releasing stops the two devices fighting over the AirPods.

## Build and run
```bash
./build.sh                 # signs with the first "Apple Development" identity, or set SIGN_IDENTITY
open build/EveryPods.app   # log: ~/Library/Logs/EveryPodsMac.log
```
It has to be a **signed app bundle**: IOBluetooth never delivers its async callbacks to an unsigned
command-line binary. The deployment target is macOS 13 (a newer target makes `open` fail with -10825).

## First-time setup
1. Pair the **AirPods** with this Mac in System Settings > Bluetooth. A stale pairing fails with `KEY_NOT_FOUND`.
2. Pair the **Android phone** with this Mac the same way, and enable Cross-Device in the Android app.
3. In the menu choose **Phone** and **AirPods** from the paired devices.
4. In the Android app add this Mac as a peer, then choose **Authorize with Phone...**. This runs the one-off setup
   exchange; without it the phone closes the channel ("Rejecting unconfigured peer").
5. Allow **Accessibility** (menu: *Allow Pausing Playback*) so the app can send the play/pause key.

## How it works
| File | Role |
|---|---|
| `Protocol.swift` | The 4-byte cross-device packets and service UUIDs |
| `ServiceLookup.swift`, `PeerLink.swift` | Finds the phone's RFCOMM channel and keeps the link up with backoff |
| `SetupAuthorizer.swift` | One-off two-exchange authorization (`BtHandoverSync.kt` format) |
| `AirPodsConnector.swift` | Connects and disconnects the AirPods |
| `MediaWatcher.swift` | CoreAudio per-process flags: who is playing, who is using the microphone |
| `MediaControl.swift` | Pauses via the system play/pause key |
| `HandoverController.swift` | The state machine tying it together |
| `main.swift` | Menu-bar UI, sleep/wake handling, launch at login |

Measured behaviour worth knowing: playback shows up in CoreAudio within about a second, but after a pause the
flag lingers 10-15 s (browsers keep the stream open), so "started" is a good trigger and "stopped" is slow.

## Known gaps
- Only the 4-byte protocol is implemented, not the V2 peer frames.
- No handling of the phone's AirPods data frames (battery, ear detection, AACP control).
- Not notarized; not yet tested across sleep/wake or a long soak.
- The menu pause key goes to whichever app owns the system media session.

## Out of scope: AirPods control

The app does not talk to the AirPods' control channel (battery, ear detection, listening modes). macOS's
own Bluetooth daemon holds it (AACP, L2CAP 0x1001) and the AirPods serve one client only; opening it from
this app got no replies and disturbed the system's session. macOS has native support for those features.

## Stem press during calls

Checked with AirPods Pro in a Google Meet call in Chrome: a stem press does not mute the call, and it
reaches this app through none of the usual paths (no system media key, no event on the AirPods' HID
collections, nothing in the Bluetooth daemon's log). macOS handles it internally, so only apps that adopt
Apple's system mute API respond. There is nothing for this app to hook.
