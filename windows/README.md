# LibrePods — Windows

Minimal Windows companion for LibrePods. Implements cross-device handover only:
when you start media on Windows, the AirPods migrate from your paired Android
device; when Android needs them back (incoming call, media, etc.), they migrate
back. No ANC / battery / ear-detection on Windows — those live on Android.

## Requirements
- Windows 10 1803 or newer (Windows 11 recommended)
- Bluetooth + classic-RFCOMM-capable adapter
- Visual Studio 2022 Build Tools (or full IDE) with the "Desktop development with C++" workload and Windows 10/11 SDK
- CMake 3.20+

## Build
```powershell
cd windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
The resulting binary is `build/Release/LibrePods.exe`.

## First-time setup
1. Pair AirPods with Windows (Settings → Bluetooth & devices → Add).
2. Pair your Android phone with Windows (same place).
3. Enable Cross-Device in the LibrePods Android app.
4. Run `LibrePods.exe`. The tray icon appears. On first launch it auto-discovers
   both peers and writes `%APPDATA%\LibrePods\config.txt`.
5. If discovery fails: right-click tray → *Pair with Android...* and *Select AirPods...*

## How it works
LibrePods talks to Android's existing `1abbb9a4-10e4-…-5471342` RFCOMM service
using the 4-byte `CrossDevicePackets` protocol (see
[CrossDevice.kt](../android/app/src/main/java/io/nikos/everypods/utils/CrossDevice.kt)).
Windows does not speak AACP/L2CAP to the AirPods themselves — it relies on the
OS A2DP/HFP profiles, so the AirPods appear as a normal Bluetooth headset.

| Direction | Packet | Behavior |
| --- | --- | --- |
| Windows → Android | `kRequestDisconnect` | Windows started media, please release AirPods |
| Android → Windows | `kRequestDisconnect` | Android needs them back, please release AirPods |
| Either way | `kAirPodsConnected` / `kAirPodsDisconnected` | Ownership status broadcast |
| Either way | `kPeerAudioActive` / `kPeerAudioIdle` | The sender is/is-not in a *call* — the receiver holds off its own call takeover (see ../docs/peer-link.md) |

Multiple peers can be connected simultaneously (Pixel + Xiaomi, for example);
`kRequestDisconnect` is broadcast to *all* peers, and any peer can claim the
AirPods via `kAirPodsConnected`. The first peer to acknowledge release wins.

### Audio routing
When AirPods come up on Windows, the OS sometimes auto-promotes them to the
default render device, and sometimes doesn't (depends on the per-role
`level` / `rank` state — see
[Microsoft Learn: Default Audio Endpoint Selection](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/default-audio-endpoint-selection)).
LibrePods runs an event-driven `IMMNotificationClient`: when the AirPods
endpoint transitions to `DEVICE_STATE_ACTIVE`, it calls
`IPolicyConfigVista::SetDefaultEndpoint` (the undocumented API every audio-
switcher utility uses) across all three roles — *but only when the endpoint
is not already the default* for that role. This avoids a brief audio glitch
when the OS has already auto-promoted us.

While Windows holds ownership (`OwnershipState::LocalPc`), the notifier stays
armed in "persistent" mode: any UNPLUGGED → ACTIVE bounce (Apple auto-switch
flutter; brief BT blip) re-applies routing automatically.

### When does Windows reject a peer takeover?
A peer's `kRequestDisconnect` is rejected only when a **real-time
communications app** has an active audio session on the AirPods. The
allowlist is in `AirPodsConnector.cpp::isCommsAppExe()` and currently covers:

| App | Process |
| --- | --- |
| Microsoft Teams (new) | `ms-teams.exe` |
| Microsoft Teams (classic) | `Teams.exe` |
| Zoom desktop | `zoom.exe` |
| Discord | `Discord.exe` |
| Slack | `slack.exe` |
| Cisco WebEx | `webex.exe`, `webexd.exe` |

An open microphone in an app set to **keep the AirPods while it uses the mic**
(see below: Teams, Zoom, Webex, Slack, Discord and the browsers by default) also rejects it, and tells phones a call is running so they
don't take the AirPods for music or their own call.

Passive media (YouTube, Spotify, browser audio, system sounds) does **not**
trigger the reject. If you're listening to music on the PC and pick up your
phone to take a call, the AirPods migrate to the phone instantly.

### Apps that take or keep the AirPods
Tray menu → **Apps that take or keep the AirPods** lists, sorted by name, the
built-in apps (even before they make a sound), every app with saved settings, and
every app that used sound or the mic (the 40 most recent, remembered across
restarts). Every app has the same three independent settings, in any combination:

- **Take the AirPods when it opens the mic**: follows "Take AirPods when a call
  starts on this PC".
- **Take the AirPods when it starts playing**: only a start in the Windows media
  controls counts, so notification dings never take the AirPods (VLC, outside
  the media controls, counts when its audio starts). Follows "Take AirPods when
  this PC starts playing media".
- **Keep the AirPods while it uses the mic**: while the app has the default mic
  open it is a call. Phones and the Mac are told so and don't take the AirPods
  (for music or their own call), and Windows refuses their requests.

With nothing ticked, the app never takes or keeps the AirPods. Each line shows
the settings in force, e.g. "chrome.exe (built in): takes the AirPods when it
starts playing; keeps the AirPods while it uses the mic". Clicking a setting saves
it and keeps the menu open (it reopens at the same spot); actions such as Connect
AirPods now and Quit close it.

Where an app's settings come from, first match wins:
1. what you saved (built-ins included: their line then says "(built in,
   changed)", and **Reset to built-in** in their submenu undoes it);
2. the built-in catalog:

   | Apps | Pre-ticked |
   | --- | --- |
   | Teams (`ms-teams.exe`, `Teams.exe`) | opens the mic, keep |
   | Zoom, Webex, Slack, Discord | keep |
   | Chrome, Edge, Firefox, Brave | starts playing, keep |
   | Spotify, VLC | starts playing |

3. all three, for an app first seen using the mic and sound together (a call app
   works out of the box; saved at that moment);
4. **Take the AirPods when it starts playing**.

Saved in `%APPDATA%\Propods\config.txt`: an `appset=name.exe` line per app with
saved settings, plus `takeonmic=`, `takeonplay=` and `keeponmic=` lines for each
ticked setting; remembered apps are `seenapp=` lines. Lines from earlier test
builds still load (`callapp=` = mic + keep, `protectapp=` = keep, `mediaapp=` =
play, `nevertake=` = nothing ticked).

### Multi-window pause
On a Windows → phone handover, all three GSMTC pause methods (current
session, all sessions, media-key broadcast) plus an `EnumWindows`-based
`WM_APPCOMMAND` pass to every top-level Chrome/Edge/Firefox window catch
YouTube/Spotify even when they're playing in an unfocused tab or on a
second monitor.

### Diagnostic logging
When `BluetoothSetServiceState` fails, a `[BT-diag]` block dumps
`BluetoothGetDeviceInfo` flags, the full `BluetoothEnumerateInstalledServices`
GUID list, and the WinRT `ConnectionStatus`. This makes it possible to tell
at a glance whether the device is paired, currently connected, and whether
A2DP/HFP are even advertised.

## Limitations (deferred)
- Battery / ANC / ear-detection UI (would need AACP/L2CAP)
- Auto-start on login (registry entry)
- MSI installer + code signing
- Browser-hosted call detection (Teams web, Google Meet, WebEx web). The
  current comms-app allowlist matches process names, which can't
  discriminate browser tabs running calls from browser tabs playing music.
