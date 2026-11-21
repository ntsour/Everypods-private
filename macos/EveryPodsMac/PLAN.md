# EveryPods for Mac — port plan

Goal: the same user experience and end behaviour as the Windows client (`windows/`), rebuilt natively for
macOS. The Windows client is the specification; this document says what is carried over as is, what is
re-implemented for macOS, and where the Mac intentionally differs.

Scope is Windows parity only: cross-device handover with the phone(s), call-aware behaviour, audio routing,
tray/menu UX. No AirPods control (battery, ANC, ear detection) — macOS owns that channel (see README).

## 1. What the user gets (parity checklist)

| Windows behaviour | Mac |
| --- | --- |
| Tray icon with ownership tooltip ("on this PC / on Android / unknown") | Menu-bar icon, state-aware, tooltip + first menu line |
| Menu: ownership line, AirPods Bluetooth state (checkmark) | Same |
| Menu: list of Android peers with connected checkmark, per-peer Remove | Same |
| Menu: Add Android peer…, Select AirPods…, Connect AirPods now | Same (Add peer also runs the one-time phone authorization) |
| Menu: Auto-set AirPods as audio device (toggle) | Same (CoreAudio default output) |
| Menu: Start with login (toggle), Quit | Same (SMAppService) |
| First run: auto-discover phones and AirPods from paired devices | Same |
| Single instance | Same |
| Media starts on PC → AirPods move from phone to PC | Same (CoreAudio per-process output) |
| Phone asks for AirPods (music: `RequestDisconnect`, call: `RequestHandover`) → PC releases | Same; the holder always yields |
| Several phones at once; request broadcast to all, first ack wins | Same |
| Survive reconnects, sleep, brief Bluetooth drops | Same, plus wake rebuild |

## 2. Architecture

Windows is layered (`PeerRegistry` / `AirPodsConnector` / watchers / `HandoverController` / `TrayIcon` /
`SettingsStore`). The Mac keeps that layering so the two stay comparable. Everything runs on the main thread
(IOBluetooth callbacks and timers are main-run-loop); no locks needed.

| Module | Windows source | Mac file | Notes |
| --- | --- | --- | --- |
| Protocol | `common/crossdevice_protocol.hpp` | `Protocol.swift` | Same packets and classifier |
| Settings | `SettingsStore` | `Settings.swift` | UserDefaults; list of phones, AirPods, autoRoute, autoPause |
| Peer link | `BluetoothRfcommClient` | `PeerLink.swift` | RFCOMM client, backoff 1.5s→15s (matches Windows) |
| Peer registry | `PeerRegistry` | `PeerRegistry.swift` | N links, broadcast, names, add/remove |
| Peer discovery | `discoverAndroidPeers` | `PeerDiscovery.swift` | Paired devices whose SDP has the CrossDevice UUID |
| AirPods | `AirPodsConnector` | `AirPodsConnector.swift` | connect with 3 attempts (0 / 1.5 s / 3 s), disconnect, `isConnected` |
| Audio routing | `setAsDefaultAudioDevice` + persistent arm | `AudioRouter.swift` | CoreAudio default output; re-apply while we hold the AirPods |
| Media | `MediaPlaybackWatcher` | `MediaWatcher.swift` | Per-process CoreAudio output (no GSMTC on macOS) |
| Calls | `TeamsCallWatcher` + `hasActiveCallSessions` | `MediaWatcher.swift` | Comms apps, mic+output, browser meeting tabs |
| In-case probe | `BleProximityWatcher` | `ProximityWatcher.swift` | CoreBluetooth scan, same Apple 0x004C parse, same rules |
| Controller | `HandoverController` | `HandoverController.swift` | Direct port of the state machine and its timers |
| Menu | `TrayIcon` | `main.swift` + `Menu.swift` | NSStatusItem |
| Authorization | (none; Mac-only) | `SetupAuthorizer.swift` | Needed because newer Android builds require it |

## 3. Algorithms carried over unchanged

The controller is ported rule for rule, with the same constants:

- Ownership: `unknown / thisMac / otherDevice`. `setState` returns whether it changed; packets are sent only on
  a real transition.
- Debounce 300 ms on media-start; one takeover in flight at a time.
- Anti-ping-pong: ignore a media trigger for 3 s after losing the AirPods; reject a peer's `RequestDisconnect`
  for 3 s after our own takeover (re-assert `AirPodsConnected`).
- Voluntary-release gate and the 30 s same-app suppression window (a player that auto-resumes after we handed
  the AirPods over does not take them back); cleared when we claim, when a call starts, when playback stops
  (outside the 5 s post-release window), when the call ends.
- Requester always wins: `RequestDisconnect` and `RequestHandover` are both accepted by the holder. (The Windows
  code also has a call-hold veto for `RequestDisconnect`, but its own comment says it was removed for the
  "requester always wins" design and the Android app force-connects after 6 s anyway, so it is not ported.)
  The only rejection is the 3 s anti-ping-pong right after our own takeover.
- Not the holder → reply `AirPodsDisconnected` and do nothing else.
- Takeover = broadcast `RequestDisconnect`, start connecting at once (not after the ack), retry connect 3 times
  at 0 / 1.5 s / 3 s; failure arms the anti-ping-pong window. (Windows waits on the ack only as an
  accelerator; here the first connect attempt is event-driven by the ack with a short cap.)
- Bluetooth edge observer (1 s poll): rising edge claims ownership, announces, routes audio; falling edge
  while holding → release, announce, arm anti-ping-pong.
- On phone connect: sync state to the phone only if it changed; if we held the AirPods and they are gone
  (phone grabbed them before RFCOMM was up), reclaim.
- In-case probe: block a *media/call-triggered* takeover only when the freshest beacon is positively
  "lid closed and neither pod in ear"; fail open; manual Connect bypasses it. RSSI floor and 12 s stale window
  as Windows.

## 4. Re-implemented for macOS (different mechanism, same behaviour)

- **Bluetooth**: IOBluetooth RFCOMM client + SDP (works only in a signed `.app`; see README).
- **What is playing**: CoreAudio process objects (macOS 14.4+). Playback shows within ~1 s; stops lag 10–15 s
  (browsers keep the stream), so "started" is the trigger and "stopped" is used only for gate clearing.
- **Calls**: Windows matches comms-app process names on the AirPods endpoint. Mac equivalent: a process with
  mic and output at once, a native call app playing, or a Chromium browser playing with a meeting tab open
  (joined muted). Recorders are excluded.
- **Audio routing**: CoreAudio `DefaultOutputDevice`/`DefaultSystemOutputDevice` set to the AirPods, only when
  not already default; re-applied while we hold the AirPods.
- **Start at login**: `SMAppService`. **Single instance**: bundle-id check at launch.

## 5. Deliberate differences

1. **Pause on release** (setting, default on). Windows never touches playback; on the Mac the pause key was
   tested and works, and without it audio jumps to the speakers. Toggle: "Pause playback when handing over".
2. **Reports call state to the phone** (`peerAudioActive/Idle`). Windows never sends it. Newer Android
   builds use the flag to suppress takeover of a ringing call, which the Mac verified.
3. **Authorization step** with the phone (setup service). Windows predates it.
4. Auto-take toggle ("Take AirPods when this Mac plays audio") stays as a setting; Windows is always on.

## 6. Delivery phases

1. **Core**: Settings, PeerRegistry/Discovery, AirPodsConnector retries, HandoverController port.
2. **Detection and routing**: ProximityWatcher, AudioRouter.
3. **UX**: menu parity, first-run discovery, Add/Remove peer, Select AirPods, Connect now, toggles, icon states,
   single instance, tooltips.
4. **Verify on hardware** (Pixel + AirPods Pro): each row of the parity table; then commit and update PR #7.

Acceptance, per phase 4: Mac plays → AirPods move; phone plays/calls → AirPods move back (requests always accepted); no ping-pong; two phones; phone link recovers after wake and
Bluetooth toggles; in-case AirPods do not trigger a takeover; Start at login works.

Out of scope here: Developer ID signing/notarization and packaging (last), the long soak test (later),
AirPods control.
