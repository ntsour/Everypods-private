# EveryPods for Mac

A menu-bar app that makes your AirPods follow you between a Mac and an Android phone running EveryPods.
It is the macOS counterpart of the Windows tray client (`../../windows`): same cross-device protocol,
same idea ("AirPods follow attention"), built on IOBluetooth and CoreAudio.

## What it does
Same behaviour as the Windows client (see [PLAN.md](PLAN.md) for the full mapping):
- Keeps a Bluetooth link to every Android phone you added and answers their status pings.
- **A phone asks for the AirPods** (it started audio or a call): the Mac releases them (pausing playback first, a
  setting) and does not grab them straight back.
- **The Mac starts playing audio or a call**: asks the phones to release them and connects them here, unless the
  AirPods appear to be in their case. Sets them as the audio output.
- Tells the phones when a call is active on the Mac so the Android app holds back takeovers of a ringing call.
- Guards against ping-pong (3 s after a takeover or a loss, 30 s for the same app after handing over).

## Build and run
```bash
./build.sh                 # signs with the first "Apple Development" identity, or set SIGN_IDENTITY
open build/EveryPods.app   # log: ~/Library/Logs/EveryPodsMac.log
```
It has to be a **signed app bundle**: IOBluetooth never delivers its async callbacks to an unsigned
command-line binary. The deployment target is macOS 13 (a newer target makes `open` fail with -10825).

## First-time setup
1. Pair the **AirPods** and the **Android phone** with this Mac in System Settings > Bluetooth. A stale AirPods
   pairing fails with `KEY_NOT_FOUND`: remove and pair them again.
2. In the Android app turn on Cross-Device Handover and add this Mac as a peer.
3. Start the app. On first launch it selects the paired AirPods, finds the phone and authorizes with it (the
   one-off setup exchange; without it the phone closes the channel with "Rejecting unconfigured peer").
   Later, **Add Android peer…** does the same for another phone.
4. Allow **Accessibility** (menu: *Allow Pausing Playback*) so the app can pause playback on handover, and the
   Bluetooth permission prompt (used for the in-case check).

Menu: AirPods state, Android peers (checkmark when connected; Authorize / Remove), Add Android peer…, Select
AirPods, Connect AirPods now, Release AirPods, and the toggles (take on playback, auto-set audio device, pause on
handover, start at login).

## How it works
| File | Role (Windows counterpart) |
|---|---|
| `Protocol.swift` | The 4-byte cross-device packets and service UUIDs (`crossdevice_protocol.hpp`) |
| `Settings.swift` | Phones, AirPods, toggles (`SettingsStore`) |
| `ServiceLookup.swift`, `PeerLink.swift` | Finds the phone's RFCOMM channel and keeps one link up (`BluetoothRfcommClient`) |
| `PeerRegistry.swift`, `PeerDiscovery.swift` | Several phones, discovery (`PeerRegistry`, `discoverAndroidPeers`) |
| `SetupAuthorizer.swift` | One-off two-exchange authorization (`BtHandoverSync.kt` format; Mac only) |
| `AirPodsConnector.swift` | Connects (3 attempts) and disconnects the AirPods (`AirPodsConnector`) |
| `AudioRouter.swift` | Sets the AirPods as the output device |
| `MediaWatcher.swift` | CoreAudio per-process flags: playback, microphone, calls (`MediaPlaybackWatcher`, `TeamsCallWatcher`) |
| `ProximityWatcher.swift` | In-case check from BLE beacons (`BleProximityWatcher`) |
| `MediaControl.swift` | Pauses via the system play/pause key |
| `HandoverController.swift` | The state machine (`HandoverController`) |
| `main.swift` | Menu-bar UI, sleep/wake, start at login (`TrayIcon`, `main.cpp`) |

Measured behaviour worth knowing: playback shows up in CoreAudio within about a second, but after a pause the
flag lingers 10-15 s (browsers keep the stream open), so "started" is a good trigger and "stopped" is slow.

## Known gaps
- Only the 4-byte protocol is implemented, not the V2 peer frames.
- No handling of the phone's AirPods data frames (battery, ear detection, AACP control).
- Not notarized; not yet tested across sleep/wake or a long soak.
- The pause key goes to whichever app owns the system media session.
- The in-case check has not been seen working yet: no BLE beacons were observed with the AirPods in use (it
  fails open, so it never blocks a handover).

## Out of scope: AirPods control

The app does not talk to the AirPods' control channel (battery, ear detection, listening modes). macOS's
own Bluetooth daemon holds it (AACP, L2CAP 0x1001) and the AirPods serve one client only; opening it from
this app got no replies and disturbed the system's session. macOS has native support for those features.

## Stem press during calls

Checked with AirPods Pro in a Google Meet call in Chrome: a stem press does not mute the call, and it
reaches this app through none of the usual paths (no system media key, no event on the AirPods' HID
collections, nothing in the Bluetooth daemon's log). macOS handles it internally, so only apps that adopt
Apple's system mute API respond. There is nothing for this app to hook.
