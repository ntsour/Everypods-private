# Windows client: revival analysis (2026-11-20)

The Windows tray client (`windows/`, last real change 2026-06-09) was paused for two reasons:

1. Handover to Windows was slow and unreliable.
2. AirPods would not stay connected to Windows when audio was idle; the link kept dropping.

This document checks the client against the current public EveryPods Android app
(`ntsour/Everypods` `main` @ `4fbcdc14`) and evaluates whether newer firmware or
a different technical approach can fix those two problems. Sections 1–5 were written
from code reading and web research before any hardware test. **Section 6 records what
was then measured on hardware the same day** and supersedes the hypotheses where they
differ.

---

## 1. What changed on Android since June

The Android peer layer has been rebuilt. In summary:

| Area | June (what Windows was built against) | Now |
|---|---|---|
| Transport | One legacy RFCOMM UUID `1abbb9a4-…`, 4-byte commands | Legacy UUID **still served**, plus a new v2 UUID `97ce0b4d-…` with `EPH2` length-prefixed JSON frames (HELLO, SNAPSHOT, PING/PONG, ACQUIRE_*) — `docs/peer-protocol-v2.md` |
| Peer authorization | Configured MAC was enough | Inbound sockets are accepted only if `DeviceConnectionStore.coordinationAuthorized(addr)`, i.e. a **BT handover entry that has been acknowledged** by the peer through the BT setup-sync endpoint `de51a5b3-…` (`docs/bt-handover-sync.md`) |
| Takeover requests | `REQUEST_DISCONNECT`/`REQUEST_HANDOVER` → Android releases | Same, but ignored when the Android side knows the pods are in the case (`shouldAcceptHandoverRequest`), and ignored unless `cross_device_enabled` and `handoverReady(peer)` |
| Liveness | Ad-hoc | Android server sends `REQUEST_CONNECTION_STATUS` every 20 s and **closes the legacy link if no `AIRPODS_CONNECTED/DISCONNECTED` reply arrives within 45 s** (`LegacyPeerLiveness`) |
| Framing | `read()` = one packet | Android decodes the legacy stream incrementally (`LegacyPeerStreamDecoder`); unknown payloads close the link |
| Passive (lid-open) acquisition | n/a | Requires a fresh v2 snapshot quorum from all peers; **legacy peers cannot grant it** |
| `WINDOWS_AUDIO_ACTIVE/IDLE` | Gated Android takeover | Still parsed, sets `peerAudioActive`, which nothing reads; effectively a no-op |

### 1.1 Compatibility problems for the current Windows client

Ranked by severity:

1. **Windows can probably no longer be authorized as a peer on a fresh setup (blocker).**
   Adding a BT handover entry on Android puts it in a *pending* state and calls
   `BtHandoverSync.wake()`, which dials the PC on `de51a5b3-…` and expects a
   length-prefixed `{"version":1,"revision","origin","enabled"}` reply. Windows has no
   such listener, so the entry stays pending, `handoverReady()` stays false, and the
   legacy listener rejects the PC ("Rejecting unconfigured … peer"). Only installs whose
   PC entry was *migrated* from the old `cross_device_peers` set get an authorized,
   non-pending entry.
   **Fix:** have Windows serve the BT setup-sync endpoint, which is small (one request, one
   reply, 4-byte big-endian length plus JSON, 10 s limit). Alternatively, add an
   Android-side "legacy PC" path; that is worse, because the Android code is hardened
   against unacknowledged peers on purpose.
2. **No stream framing on Windows.** `BluetoothRfcommClient` hands each
   `LoadAsync` result to `crossdevice::classify()`, which uses `equals4` on the whole
   buffer. If two packets arrive together (for example a status reply right after an
   `AIRPODS_CONNECTED`), the result is `Unknown` and both packets are silently dropped.
   Windows needs the same incremental 4-byte decoder that Android now has.
3. **Role election can make Android dial the PC.** Android picks client or server by
   comparing Bluetooth *device names*. If the phone's name sorts before the PC's, Android
   starts a `CrossDeviceClient` link to the PC on both the legacy and v2 UUIDs, but the PC
   runs no RFCOMM server. The result is a retry loop. While the AirPods are on the phone,
   this means periodic paging of the PC (the code itself says the audio impact of these
   probes is unvalidated). An inbound connection from Windows pauses that link
   (`CrossDeviceClient.pauseLink`), so it mostly hurts when the PC is offline.
   **Fix:** either Windows also listens on the legacy UUID, or Android learns
   "this peer is a PC: always server" (peer kind persisted at setup).
4. **Liveness is fine as long as Windows answers.** Windows already answers
   `REQUEST_CONNECTION_STATUS` from `m_state`. Keep it answering promptly: a busy
   thread for over 45 s now gets the link closed.
5. **Passive lid-open on the phones may be degraded when a legacy PC is a member.**
   v2 passive acquisition needs every member's snapshot, and a legacy peer can't provide
   one. This needs confirming in `PassiveAcquisitionCoordinator`/`CrossDevice`. If it
   holds, a PC peer either blocks phone case-open reconnects or is ignored by them, and
   both are bad. This is the main argument for giving Windows real v2 support.
6. **Leftover holder-side veto.** Windows still rejects `kRequestDisconnect` during an
   active Teams/Zoom/etc. session (`HandoverController.cpp:383-395`). Android has a
   matching VoIP call-hold veto. Keep the two consistent, and decide explicitly whether
   the Windows veto survives.

### 1.2 Options for protocol alignment

| Option | Effort | Gets you |
|---|---|---|
| **A. Legacy-plus** (setup-sync endpoint + stream decoder + PC-aware role) | Small, about 1–2 days | Explicit handover works again with current Android |
| **B. Full v2 client** (HELLO identity, SNAPSHOT/PING-PONG heartbeat, ACQUIRE_* grants) | Medium, about 1–2 weeks | Windows becomes a first-class member; phone lid-open quorum works with a PC present; ownership is measured, not guessed |
| C. Android "legacy PC" special-case | Small, but on Android | Undoes some of the connection-hardening work. Not recommended |

Recommendation: **do A first** to get a working baseline and measure the two real
problems, **then B**. v2 framing is simple (`EPH2`, version, type, u32 length, JSON) and
WinRT `StreamSocket` handles it easily. The hard part is the reservation state machine;
port it from the Kotlin tests rather than reinventing it.

---

## 2. Problem 1: slow, unreliable handover to Windows

### What the code does today

`AirPodsConnector::connect()` has no real "connect the headset" call. It *wakes* the link
indirectly:

1. `GetRfcommServicesAsync(Uncached)`: an uncached SDP query, which the code itself
   notes takes **10–15 s**.
2. Opens an RFCOMM socket to the AirPods' first RFCOMM service, with a 3 s timeout.
3. Relies on Windows noticing the ACL and bringing up A2DP/HFP by itself.

The commit history shows the ways this failed:
- Toggling profiles with `BluetoothSetServiceState` broke audio, left endpoints
  `UNPLUGGED`/`NOTPRESENT`, and Windows persisted the change (needing a re-pair). It was removed.
- Rapid back-to-back handovers: "the AirPods idle-drop and are slow to re-wake (may need a
  second trigger)".
- Disconnect is passive. Windows never actually releases; it waits for the phone's A2DP
  grab to push it off.

### Better approach: let the Bluetooth audio driver connect

Windows exposes the same connect and disconnect that the Settings "Connect" button uses,
through the audio stack rather than the Bluetooth API:

- `KSPROPSETID_BtAudio` → **`KSPROPERTY_ONESHOT_RECONNECT`** / **`KSPROPERTY_ONESHOT_DISCONNECT`**
  (`ksmedia.h`, Windows 7+). The property is sent to the Bluetooth audio KS filter. For
  hands-free devices, the driver forwards it as a connect request to the HFP driver,
  which connects asynchronously.
- From user mode, this is reachable through Core Audio:
  `IMMDevice` → `IDeviceTopology` → connector → `IPart` → `Activate(IKsControl)` →
  `KsProperty(...)`. The open-source **ToothTray** tray app uses this route to
  connect and disconnect Bluetooth headphones.
- "Success" only means the driver *attempted* the connect. Confirm by watching the endpoint
  go `ACTIVE` (the client already has an `IMMNotificationClient` for this) or through the
  `KSPROPERTY_JACK_DESCRIPTION` connection state.

Expected effect, still to be measured: handover becomes "phone releases → one connect
request → endpoint ACTIVE" with no 10–15 s uncached SDP wait and no profile toggling. It
also gives Windows a **real release** (`ONESHOT_DISCONNECT`) for
`kRequestDisconnect`, so the phone no longer has to win a page race against a PC that
still holds the ACL.

Smaller improvements on top:
- Do the connect in parallel with the release request, as the June connect-first change
  already does, but with the KS reconnect instead of SDP.
- Retire `GetRfcommServicesAsync(Uncached)` entirely from the hot path.
- Instrument each handover in the log: request sent → peer ack → connect issued → ACL up
  → endpoint ACTIVE → first audio. Without these numbers, "slow" can't be acted on.

---

## 3. Problem 2: AirPods drop from Windows when idle

### What was learned in June (from commit messages)

- AirPods dropped the link about **22 s** after audio stopped. The idle timer resets on
  application-layer traffic (A2DP audio, SDP responses), **not** on an open but silent
  RFCOMM/L2CAP channel (`db02b3c`).
- SDP-probe keepalives queued behind other radio activity and missed the window. A
  persistent RFCOMM socket hit `WSAEADDRINUSE` after a drop and couldn't self-heal
  (`3b4984e`).
- Polling audio endpoints made Windows disconnect even sooner, at about 45 s instead of
  about 2 min (`4044b91`). All keepalives were then removed.

### Most likely root cause (hypothesis, to verify first)

A normal Windows-to-AirPods pairing stays connected while idle, because the
**HFP service-level connection** (and AVDTP signalling) keeps the ACL in use. Because the
client brought the link up with an SDP/RFCOMM wake instead of a driver-level profile
connect, the AirPods may have ended up with a bare or partial link and no HFP SLC. That
would explain the idle drop.

There is also a second suspect: **the phone taking them back.** Android's own reconnect
paths (system auto-reconnect, EveryPods lid-open/passive recovery, A2DP retry) can page
the AirPods while Windows is idle and win. With the current Android code, a legacy PC peer
also can't answer v2 snapshots, so Android has less information about who holds the pods.
A short-lived "drop" from Windows could therefore be a steal.

How to tell them apart (cheap, do this first):
1. Pair the AirPods to the PC with **no phone nearby** (or phone Bluetooth off) and the
   client **not running**. Leave them idle in-ear for 10 min. If they stay connected,
   Windows plus firmware is fine at baseline.
2. Repeat with the phone on but EveryPods disabled. Then with EveryPods enabled. Then
   with the client handing over. Whichever step reintroduces the drop is the cause.
3. Capture the Windows Bluetooth ETW trace (`BthMini`, `BthA2dp`, `BthHfAud`) or the
   btvs/Wireshark sniffer log for the drop. It shows *who* sent the disconnect and the
   reason code.

### Candidate fixes, in order

1. **Connect through the driver (`ONESHOT_RECONNECT`, see §2).** If the hypothesis is
   right, this alone fixes idle drops, because the AirPods get a full HFP plus A2DP
   connection like a normal pairing.
2. **Stop the phone from stealing.** Make the Windows peer authoritative while it holds
   the pods. Either Windows speaks v2 snapshots (ownership `held`), or, with legacy, keep
   answering status with `AIRPODS_CONNECTED` and make sure Android respects a legacy
   holder in its passive paths.
3. **Last resort: a silent render stream.** While Windows owns the AirPods *and* the
   user wants "stay connected", keep a WASAPI shared-mode stream open on the AirPods
   endpoint, writing near-silence. This is the same trick some "keep speaker awake"
   utilities use. Measured cost: it keeps A2DP streaming, so AirPods battery drain rises
   and the AirPods look "in use". Make it opt-in and time-boxed (for example 30 min after
   last audio).
4. Avoid any form of periodic SDP or COM endpoint polling. June showed it makes things
   worse.

---

## 4. Does newer AirPods firmware help?

Current public firmware for H2 AirPods (Pro 2, Pro 3, AirPods 4/5, Max 2) is
**9B47 (2026-11-10)**, replacing 9A348. Apple's notes only say "bug fixes and other
improvements". There is no public statement about multipoint, idle timers or non-Apple
source behaviour.

So there is **no documented reason** to expect firmware to fix either problem. It is
still worth one test run, because it's cheap. Notes:
- Firmware can only be updated from an iPhone, iPad or Mac (or by leaving them charging
  near one). It can't be done from Android or Windows.
- Record the firmware version in every test log, and run the §3 baseline test on 9B47
  before and after any code change. Otherwise firmware and code effects can't be
  separated.

---

## 5. Proposed plan

| Step | What | Outcome |
|---|---|---|
| 0 | Fix Windows CI so the `.exe` builds (branch `chore/windows-ci-build`; no run has triggered yet) | A downloadable build |
| 1 | Baseline test from §3 (no client, no phone; then add each layer) on firmware 9B47 | Know whether idle drop is Windows/firmware, phone stealing, or the client |
| 2 | **Spike: `KSPROPERTY_ONESHOT_RECONNECT/DISCONNECT`** in a standalone tool (like `windows/tools/bt_wake_test.cpp`) | Measure connect latency and idle stability versus the SDP wake |
| 3 | Legacy-plus compatibility (§1.2 A): setup-sync endpoint, 4-byte stream decoder, PC-aware role | Client works with the current Android app |
| 4 | Swap the `AirPodsConnector` connect/disconnect to KS properties; add handover timing logs | Problems 1 and 2 addressed if the spike holds up |
| 5 | Full v2 peer (§1.2 B) | Windows is a first-class member; phone lid-open works with a PC present |
| 6 | Opt-in silent-stream keepalive, only if step 4 doesn't fix idle drops | Fallback |

Steps 1 and 2 are the decision point. If the driver-level connect doesn't beat the
current approach on real hardware, revisit before investing in v2.

## Sources

- Android: `docs/peer-protocol-v2.md`, `docs/bt-handover-sync.md`,
  `docs/reconnect-protocol-design.md`, `utils/CrossDevice.kt`, `utils/CrossDeviceClient.kt`,
  `utils/peer/Legacy*.kt`, `relay/DeviceConnectionStore.kt`, `relay/BtHandoverSync.kt`
- Windows: `windows/src/*`, `docs/windows-handover-plan.md`, `docs/windows-multipeer.md`,
  commit history of `windows/` (2026-05-17 → 2026-06-09)
- [KSPROPERTY_ONESHOT_RECONNECT (Microsoft Learn)](https://learn.microsoft.com/en-ca/windows-hardware/drivers/audio/ksproperty-oneshot-reconnect)
- [KSPROPSETID_BtAudio (Microsoft Learn)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/kspropsetid-btaudio)
- [HFP device connection (Microsoft Learn)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/hfp-device-connection)
- [Using IKsControl to access audio properties (Microsoft Learn)](https://learn.microsoft.com/en-ie/Windows/Win32/coreaudio/using-the-ikscontrol-interface-to-access-audio-properties)
- [ToothTray (GitHub)](https://github.com/m2jean/ToothTray)
- [Apple: About firmware updates for AirPods](https://support.apple.com/en-us/106340)
- [MacRumors: Apple releases AirPods firmware 9B47](https://www.macrumors.com/2026/11/10/apple-releases-airpods-firmware-9b47/)
- [Microsoft Support: Bluetooth keeps disconnecting in Windows](https://support.microsoft.com/windows/hardware/bluetooth/bluetooth-keeps-disconnecting-in-windows)

---

## 6. Results on hardware (2026-11-20)

Setup: Pixel 10 (EveryPods 1.0.8 + `feat/handover-trace-log` + `fix/never-dial-computer-peers`),
Windows 11 laptop "TE99PWYX3P2P7BZ" (Propods from `feat/windows-handover-trace`),
AirPods Pro, firmware not recorded. Phone and PC clocks differ by about 6 s; times
below are within one device's clock. Traces: `~/Everypods-logs/20261120-*`.

### 6.1 Peer acceptance (§1.1 item 1): confirmed and fixed
- The Pixel rejected the PC 181 times in the first trace ("Rejecting unconfigured
  EveryPodsCrossDevice peer"). The PC had never completed the BT handover setup.
- Propods now answers the setup exchange on `de51a5b3-…` (`BtHandoverSetup.cpp`).
  After adding the PC on the Pixel: "BT handover synchronized" within seconds, and no
  rejections since.
- Side effect fixed: every rejected reconnect had made Windows "RECLAIM" the AirPods,
  so they flapped between devices. Gone once the link was accepted.

### 6.2 Slow handover (§2): fixed with the driver-level connect
`ks_connect_spike` showed `IPart::Activate(IKsControl)` returns `E_NOINTERFACE` on this
PC; opening the KS filter (`IMMDevice` from the connector id, or its device interface
with `IOCTL_KS_PROPERTY`) works. `KSPROPSETID_BtAudio` is supported on the AirPods'
A2DP (`bthenum#{0000110b…}`) and hands-free (`bthhfenum#bthhfpaudio…`) filters.

| Measurement | Before (SDP/RFCOMM wake) | After (ONESHOT_RECONNECT/DISCONNECT) |
|---|---|---|
| Spike, connect → stereo ACTIVE | — | median 5.3 s (4.8–7.7 s), 4/5 |
| Spike, disconnect → released | — | median 0.5 s |
| Propods, phone → PC, request → audio ACTIVE | ~16 s (link 2.2 s + ~13 s endpoint wait) | 6.7 s, 5.9 s, 7.4 s (link 1.4–3.8 s) |
| Propods, PC → phone, phone playing | 4.2–6.3 s | 4.7–5.1 s (PC releases in ~0.3 s) |

The remaining PC → phone time is the phone connecting its own audio.

### 6.3 Idle drops (§3): not reproduced after the driver-level connect
- Spike: `connect` then `watch` for 5 min with nothing played: 0 drops.
- Propods: 30 s of audio, pause, then 7½ min idle: AirPods stayed on the PC; the Pixel
  respected it (`skip reason=peer_holding`). June's client lost them ~22 s after audio
  stopped. This supports the §3 hypothesis that the old wake left the AirPods without a
  full profile connection.

### 6.4 Pixel ↔ PC link drops (new)
- ~94 of 97 drops in 50 min came from the Pixel dialing the PC on the CrossDevice UUID
  every ~15 s (name-based role election made the phone the client; the PC never
  listens). Fixed in `ntsour/Everypods` `fix/never-dial-computer-peers`: computers are
  always server-only. After the fix: 0 dial attempts.
- The rest are radio supervision timeouts (HCI reason 0x08). Before restarting the
  Pixel's Bluetooth: every 30–55 s. After the restart: none in 7 min without the
  AirPods on the PC; 2 in 10 min with the PC holding/streaming to them. A full HCI
  capture of one is in progress. Main suspect: the PC radio serving the AirPods, the
  phone link and Propods' continuous LE scan at once.
- After each reconnect the Pixel briefly tries HFP/A2DP to the PC (Android treating the
  PC as an audio device). Turning off "Phone calls" and "Media audio" for the PC in the
  Pixel's Bluetooth settings stops it.

### 6.5 Still open
- Passive (lid-open / in-ear) acquisition on the Pixel is blocked while the PC is a
  member: `passive acquisition blocked reason=peer_identity_missing` (§1.1 item 5,
  confirmed). Needs v2 (§1.2 B) in Propods.
- Windows packet framing (§1.1 item 2): the Pixel's battery/status packets
  (`00 05 00 01 …`) arrive as "Unknown packet".
- Clean up `windows/prebuilt` binaries before merging the Windows PR.

### 6.6 Tooling added
- Durable per-run Windows traces (`%LOCALAPPDATA%\EveryPods\traces`), opt-in full
  Android trace (Debug screen), "Send traces to phone" (dedicated RFCOMM service, only
  new or grown files), `tools/fetch-traces.sh` in `ntsour/Everypods` to pull both over
  adb, and the `ks_connect_spike` tool.

