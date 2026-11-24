# Playback Hold (Windows)

Hold the PC's media while the AirPods connect, so nothing plays from the PC speakers
during a handover to the PC and playback starts **once**, on the AirPods. Show the
handover's progress in a small panel, with **Retry** / **Play on PC** on failure.
Calls get progress only and are never held.

Status: implemented, **not built or tested on hardware yet**. Behind the tray setting
*Hold PC audio while AirPods connect*, off by default. Same shape as Android
(`docs/playback-hold.md` in the Everypods repo) and the Mac (`PlaybackHold.swift`).

## Problem

Today on Windows:

1. The user presses play. Audio goes to the **speakers** at once.
2. GSMTC reports the session Playing ~0.1–0.5 s later; `HandoverController` asks peers
   to release and connects (up to 5 back-to-back attempts, ~6 s page each).
3. Once the link is up, `setAsDefaultAudioDevice()` routes; Windows opens A2DP 1–3 s later.
4. The speakers played all along: typically 2–6 s, far longer on retries.

Windows doesn't pause apps when an output is *added*; apps that follow the default
device reopen their stream (100–500 ms gap), apps bound to one device keep playing on the
speakers. EveryPods itself never paused anything (pausing the *focused* GSMTC session
and resuming after a slow connect froze YouTube in the past).

## Design

| Step | What | Where |
|---|---|---|
| Pre-mute | A browser/media app's WASAPI session goes Active on a render endpoint, a media takeover is likely (`HandoverController::mediaTakeoverLikely`), not a comms app → mute **that session** (`ISimpleAudioVolume`), at most 1.5 s | `TeamsCallWatcher::setOnRenderSessionActive` → `PlaybackHold::onRenderSessionActive` → `SessionMute::mute` |
| Start | The takeover passed every gate → mute the triggering app's active sessions, **TryPauseAsync on the exact GSMTC session that started playing** (never `GetCurrentSession`) | `HandoverController::requestTakeover` → `PlaybackHold::start` |
| Unmute | The app reports Paused, or the 1.5 s cap | `MediaPlaybackWatcher::setOnSessionPlayback` |
| Release | AirPods stereo endpoint is the default render device and ACTIVE (polled 150 ms, up to 6 s, then accepted) → wait 300 ms → TryPlayAsync on the same session; one safety replay 800 ms later | `AirPodsConnector::isDefaultRenderDevice` |

### Phases

| Phase | Entered when |
|---|---|
| `RequestingRelease` | Hold starts and a peer is connected (`kRequestDisconnect` sent) |
| `Connecting` | Peer acked (`kAirPodsDisconnected`), 1.5 s passed, no peer, or Retry |
| `Routing` | Connect thread succeeded, or the BT rising-edge watcher saw the link |
| `Connected` | AirPods are the default output (calls: immediately on link) |
| `Failed` | 15 s attempt timeout, or every connect attempt failed |

Panel text: "Asking your phone to let go…", "Connecting to {name}…" (calls: "Moving your
call to {name}…"), "Switching audio…" / "Switching call audio…", "Connected",
"Couldn't connect to {name}". Progress is time-based per phase band, as on Mac/Android.

The attempt timeout is 15 s (Android/Mac: 10 s) because each Windows connect attempt
already waits a full ~6 s page.

### Failure and choices

The media stays paused. The panel shows **Retry**, **Play on PC** (calls: **Use PC**) and ×.

- **Retry**: new attempt window; `HandoverController::retryTakeover` skips the BLE probe,
  anti-pingpong and same-app gates (the user asked). If attempts are still running it just
  re-sends `kRequestDisconnect` and keeps waiting.
- **Play on PC**: stop remaining connect attempts (`cancelTakeover`), unmute, TryPlayAsync,
  and suppress media triggers for 5 s so the app's own resume doesn't start a new takeover.
- **×** or no choice within 12 s: end the hold, media stays paused.

### Cancel

- User presses play in the app during the hold (Playing not caused by us, after we saw
  Paused): end the hold, suppress triggers 5 s; the connection carries on.
- A peer takes the AirPods (not during our own connect attempts) or we release them: end
  quietly, media stays paused.
- A call during a media hold: media stays paused, the call owns the panel.

### When there is no hold

- Setting off: no media hold and no media panel (calls still show progress).
- Auto-route off: no media hold (the release condition might never come).
- Apps outside the media controls (VLC 3, games): no session to pause; progress only
  (VLC is not pre-muted either).
- The AirPods are not in use on a peer: no hold, no pre-mute, no panel, for media and
  calls. Both are required: a linked peer's v2 snapshot says `held`
  (`PeerV2Mesh::anyPeerHoldsAirPods`), **and** a BLE beacon in the last 20 s says a bud
  is in an ear (`BleProximityWatcher::budLikelyWorn`, Android's `airPodsLikelyWorn`).
  A phone reports `held` while its Bluetooth profile is up, which stays true with the
  AirPods in their case; the BLE in-case check fails open once a closed case goes quiet;
  and `kAirPodsDisconnected` is only sent around a handover. Unknown counts as not worn
  (only a PC whose BLE scan can't start skips the worn check). The takeover still runs
  quietly.

## Mute safety

Windows remembers per-app mute, so:

- Every mute is recorded in `%APPDATA%\Propods\hold-muted-apps.txt` **before** `SetMute`.
  `SessionMute::restoreLeftovers()` undoes leftovers at the next start.
- Never mutes a session that is already muted (the user's own mute).
- Hard cap: unmute 1.5 s after the pre-mute / hold start, whatever happens.
- Restore on quit (`PlaybackHold::shutdown`) and on `WM_ENDSESSION` (the overlay window
  receives it; the tray's message-only window does not).
- A pause is inherently safe: after a crash the app is just paused.

## Panel

`HoldOverlay`: a GDI-drawn `WS_POPUP` with `WS_EX_NOACTIVATE | WS_EX_TOPMOST |
WS_EX_TOOLWINDOW`, `MA_NOACTIVATE`, shown with `SW_SHOWNOACTIVATE`, bottom-right of the work
area, light/dark theme, Windows 11 rounded corners. It never takes focus, so a full-screen
video keeps playing/fullscreen. Toasts were rejected: Do Not Disturb (auto-on during
full-screen video) hides them, and unpackaged apps need an AUMID and COM activator.

The same panel also replaces the tray toast that said PC audio was paused because the
AirPods moved to another device: "AirPods moved to another device", what was
paused/muted, an **Unmute** button when apps were muted, and ×. It hides itself after
8 s (15 s with Unmute), and a handover in progress always takes the panel over.

## Tray icon

The tray icon is an AirPods glyph drawn at run time (`AirPodsIcon.cpp`, GDI+), coloured by
status for the taskbar's light/dark theme: **green** on this PC, **blue** while a handover
to this PC runs, **grey** on another device (dimmed, like the Mac menu bar). The Mac uses
Apple's SF Symbol `airpodspro`, which may not be shipped outside Apple platforms, so
Windows draws its own version of the same subject. `Propods.ico` stays the exe's icon.

## Threading

`PlaybackHold` runs all logic on one MTA worker thread; every public method posts and
returns. GSMTC Try*Async calls wait at most 1 s. `TeamsCallWatcher` now hands new sessions
to its STA thread (the subscription list is only touched there).

## Test checklist (hardware)

- Chrome/YouTube, Edge, Spotify, Media Player: phone → PC handover, no speaker sound, one
  start on AirPods, no pause/play stutter.
- Failure: AirPods in another device that refuses → Failed → Retry / Play on PC / × / 12 s.
- Teams call: progress only, nothing muted.
- Kill EveryPods mid-hold → restart → the app is unmuted.
- Video in full-screen: the panel shows, focus and full-screen are kept.
