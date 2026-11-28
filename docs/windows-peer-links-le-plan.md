# Windows peer links off classic Bluetooth: findings, design and plan

Status: proposal, 2026-11-28. Nothing here is implemented.
- It follows `peer-link-quiet-redesign.md`, whose phases 0–3 shipped (#31–#35 here, Everypods #123–#125).
- It reopens the 22 Nov decision in Everypods `rfcomm-removal-plan.md` that Windows keeps RFCOMM to the phones.

## 1. Short answer

- **The quiet redesign is not wasted.** Its protocol work is independent of the transport. Quiet-v1, session-scoped state, push on change, the idle probe and the stats already run over LE between the phones and the Mac. A Windows LE link would reuse all of it unchanged.
- **Two redesign items stop:**
  - Phase 4 (`legacy-in-v2`, one RFCOMM link per pair) only makes RFCOMM cheaper. LE already carries legacy packets inside its one link.
  - D5 Windows (passive scan) folds into the single LE watcher below.
- **Phase 5 (Android scan mode, the unread battery packet)** is unaffected and stays low priority.
- **Don't start building the LE link yet.** Two facts must be checked first:
  - **Windows → phone LE failed on 22 Nov.** The link encrypted, but GATT discovery never finished.
  - **Nobody has checked whether an LE link avoids the drop.**

  Phase 0 (§5) settles both with a throwaway tool and no product code.

## 2. What the idle drop tests showed (27 Nov)

| # | Setup | Classic links to phones | AirPods link after the call | Drop |
|---|---|---|---|---|
| 1 | EveryPods on, phones on, AirPods taken from the Pixel | 2. PC **peripheral** on the Pad link (Role Change 20:15:35). AirPods link created by the PC with role switch off | never in sniff, no traffic | yes, 58 s after the call (reason 0x08) |
| 2 | EveryPods closed | none of ours (one other ACL, 0x0100, in sniff) | sniff after 4 s (AirPods asked) | no (3.5 min) |
| 3 | EveryPods on, phones' Bluetooth off | none (23 failed pages, normal backoff) | sniff after 40 s | no (2 min) |
| 4 | EveryPods on, phones on, links up | 2 | not captured | yes, within seconds (user report) |

**Established:** EveryPods' classic links to the phones are enough to cause the drop. Its audio handling, polling and handover are not needed.

**Not established:**
- **What exactly about those links causes it.** Test 2 had another classic ACL and no drop.
- **The leading candidate:** the PC acting as peripheral on a phone link, a scatternet that the Intel controller (driver 24.20.0.3) handles badly. Plain traffic on a second ACL is the other. Gate G1 separates them.

This matters for the fix. If the role is the cause, any classic link can trigger it, and no API lets an app pin the role on Windows or Android.

## 3. Constraints from the 22 Nov LE spike

Taken from `rfcomm-removal-plan.md` §Phase 0 (Bleak 0.22 over WinRT):

| Pair | What happened |
|---|---|
| Windows → Pixel | The LE link came up and was **encrypted, with no prompt**, so the keys exist through CTKD. WinRT never finished GATT discovery: `Unreachable` three times, then a hang. |
| Windows → Xiaomi | `FromBluetoothAddressAsync` failed at once with `E_UNEXPECTED`. The Xiaomi was seen under a private address. |
| Windows cached GATT | Stale: it showed no EveryPods service |
| `PairAsync(Encryption)` with the classic bond present | A no-op: it returned "already paired" |
| LE-only pairing | Tried only toward the **Mac**, which refuses incoming LE pairing. **Never tried toward a phone.** Android shows a pairing prompt, which macOS lacks. |

Untested explanations for the failure. G3 checks each one:
- **(a) The Android GATT server is open on both transports.** `PeerGattServer` calls `openGattServer(ctx, cb)` with no transport, so the service is also published over BR/EDR. For a classic-bonded device, Windows may try to run GATT over the classic ACL that the RFCOMM links keep up.
- **(b) Bleak, not WinRT.** Its connect path waits for a session object. Native code can use `GattSession.MaintainConnection` and uncached discovery with retries, as Microsoft documents. Others report the first `GetGattServicesAsync` on a paired LE-privacy device timing out with `Unreachable` and the retry working.
- **(c) Wrong address.** The Xiaomi needs the identity address resolved from the advertisement, not the classic address or a private one.

## 4. Options

| Option | What | Fixes the drop if… | Cost | Long-term fit |
|---|---|---|---|---|
| **A. Windows LE central** (classic bond kept, keys through CTKD) | Windows connects to the phones' existing EveryPods GATT service, as the Mac does. Windows stops using RFCOMM for peers. | An idle LE link coexists with the AirPods (G2) and discovery works (G3) | Windows: L. Android: none or tiny (transport flag). | Best. One transport for every computer, reuses quiet-v1, lets Android delete its last RFCOMM servers. |
| **A′. LE central with an LE-only bond** | As A, but the PC's classic pairing with each phone is replaced by an LE pairing, with one prompt on the phone | G2, and G3 fails only because of the classic bond | As A, plus a pairing-migration step in setup | Same as A. The PC never needed the phones' classic profiles. |
| **B. Classic links only when needed** | Windows closes its phone links while it holds the AirPods and nothing plays, and re-dials when it loses them or starts a call. A phone taking the AirPods then goes uncoordinated: the 2 s unreachable-holder rule, the PC sees the disconnect and pauses. | Closing RFCOMM also drops the ACL (G5), and links closed while the PC holds the AirPods are enough | Windows only: M | Poor. It keeps RFCOMM, loses call protection while idle and adds a page when the PC loses the AirPods. Only a stop-gap. |
| **C. Driver or power settings only** | A newer Intel driver, or turning off "Allow the computer to turn off this device" | The bug is in this driver build | none in code | Not a fix: other users' PCs will have other radios. Still worth one test (G6), because it tells us how general the problem is. |

**Recommendation:** go for A, with A′ as its fallback. Use B only if an LE link also drops the AirPods (G2 fails).

## 5. Phase 0: gates before any product code

A throwaway C++/WinRT console tool (`windows/tools/le-spike/`), the existing HCI capture script, and at most a debug-only toggle on Android.

| Gate | Question | How | Pass |
|---|---|---|---|
| **G1** Mechanism | Role or traffic? | Repeat test 4 with the capture running (it wasn't captured), then the same with **only the Pixel** linked and **only the Pad** linked. Read Role Change and Mode Change for each link. | Tells us whether the drop needs the PC to be peripheral on a phone link. It decides nothing alone, but sets what G2 must prove. |
| **G2** LE coexistence | Does an idle LE link to each phone also drop the AirPods? | The tool holds an LE connection to both phones, encrypted, with or without GATT, while EveryPods' RFCOMM peers are off (phones' classic links down). Run the standard idle test (§9) 3 times. | 0 drops in 3 runs, and the HCI shows the AirPods link in sniff after the call |
| **G3** Discovery with the classic bond | Can native WinRT reach the EveryPods service? | Variants: advertisement watcher → `FromBluetoothAddressAsync(address, type)` → `GattSession.FromDeviceIdAsync` + `MaintainConnection` → `GetGattServicesForUuidAsync(Uncached)` with 3 retries. Each run once as-is and once with an Android debug build opening the server with `TRANSPORT_LE` (explanation a). On both phones. | Subscribe + HELLO in ≤ 5 s, 10/10 tries per phone, including after a phone-app restart and Bluetooth off/on on either side. The phone sees the PC under its configured public address. |
| **G4** LE-only bond (only if G3 fails) | Does replacing the classic pairing with an LE pairing make discovery work? | On one phone: remove the pairing on the PC, `PairAsync(Encryption)` over LE, accept the phone's prompt, rerun G3 | As G3. The AirPods and the phone's other pairings are unaffected. |
| **G5** For option B only | Does closing our RFCOMM links drop the ACL? | HCI capture while EveryPods closes its links | Disconnect Complete for the phone handle within 10 s |
| **G6** Driver | Does a current Intel driver fix the drop with classic links? | Rerun test 4 after updating the driver | Recorded either way, for the README's troubleshooting section |

### Runbook

The tool is `le_spike.exe`, built by the Windows CI next to `Propods.exe` from `windows/tools/le_spike.cpp`. Run it from a normal PowerShell; it writes `le-spike-<time>.log` in the current folder.

**G1** (no tool). Use `bt-capture.ps1`, scenario "call", for three captures:
1. Both phones' Bluetooth on, Propods showing both linked, AirPods connected from Windows Bluetooth settings.
2. The same with only the Pixel's Bluetooth on.
3. The same with only the Pad's Bluetooth on.

Each time: call, hang up, wait 3 minutes or until a drop.

**G3** (phones on, Propods running, then again with Propods quit):
1. `le_spike devices`: shows whether Windows holds LE keys for the phones.
2. `le_spike scan`: lists the phones' EveryPods adverts and their address types.
3. `le_spike connect --tries 10`
4. With Propods quit: `le_spike connect --tries 10`, then `le_spike connect --classic <phone address> --tries 3` for each phone (explanation c).

Claude restarts the phone app and toggles Bluetooth over adb between runs, and reads the phones' `PeerGatt` log for the address each phone sees.

**G2** (Propods quit, AirPods connected from Windows settings):
1. `le_spike hold --minutes 12`
2. In an admin PowerShell, run `bt-capture.ps1` (call, then idle), three times within the hold.
3. Upload the three zips and the le-spike log.

**Decision:**
- G2 and G3 pass → **A**.
- G2 passes, G3 fails, G4 passes → **A′**.
- G2 fails → **B** as the shipped fix, with the LE work shelved.
- G3 and G4 both fail → **B**, and the Windows LE link is revisited only with application-layer authentication (rfcomm-removal D6 fallback 2).

## 6. Design for option A

### 6.1 One link abstraction in Windows

Today `PeerV2Mesh` owns RFCOMM sockets directly, and `PeerRegistry` owns `BluetoothRfcommClient`s for the legacy packets. The LE link carries both inside one record stream (`HELLO 0x01`, `LEGACY 0x02`, `PING/PONG 0x03/0x04`, `V2 0x05`, `SETUP 0x06/0x07`), as on the Mac.

- **`PeerLink`:** a small interface with `send(record)`, an `onRecord` callback, an `onClosed(reason)` callback and `peerInstallationId`.
- **`PeerV2Mesh`** takes sessions from any `PeerLink`. Its handshake, quiet-v1 probe, arbitration and acquisition code stays as it is: it works on frames, not sockets.
- **`PeerRegistry`** routes legacy packets as `LEGACY` records over the same link. `HandoverController` doesn't change.
- **The record codec** is a C++ port of `PeerGattRecords`, checked against the Kotlin and Swift test vectors.

### 6.2 `PeerGattCentral` (Windows)

- **One advertisement watcher** for all of Windows' LE needs:
  - passive, with filters for the EveryPods service UUID and Apple's 0x004C
  - it replaces `BleProximityWatcher`'s own scan (redesign D5)
  - it pauses on sleep.
- **Connect only to a device seen advertising.** An absent phone costs nothing: no paging, no dial timer. This removes the old "dial an absent peer forever" problem at its root.
- **Connect path** (G3 confirms the details):
  1. `BluetoothLEDevice` from the advertisement's address and type
  2. `GattSession`, with `MaintainConnection = true`
  3. uncached discovery of the service, with retries on `Unreachable`
  4. the TX `ValueChanged` handler first, then the CCCD write
  5. HELLO.
- Service and characteristic objects are kept as members, and teardown follows Bleak's order.
- **Identity:**
  - Windows learns who the phone is from HELLO's `installationId`. Configured peers are remembered by installation ID, with the Bluetooth address as the first-contact match.
  - The phone knows the PC from its resolved identity address, as it knows the Mac.
- **Liveness from events, no transport ping:**
  - **Link lost:** `BluetoothLEDevice.ConnectionStatusChanged` and `GattSession.SessionStatusChanged`.
  - **Phone app restarted under a live link:** `GattServicesChanged`, which triggers rediscovery on the same link.
  - **Last resort:** the quiet-v1 idle probe (60 s, then 2 × 15 s).
  - The Mac's 15 s transport ping is not copied.
- **Backoff** for failed connects reuses `PeerDialPolicy` (2 s → 300 s), and resets on a new advertisement after a link loss.
- **Stats:** the hourly STATS line gains LE counters (connects, failed connects, rediscoveries, link lifetimes), as Android's `PeerLinkStats` has.

### 6.3 Setup over LE

- "Add peer…" on Windows lists the devices that advertise the service and answered HELLO.
- Authorization uses `SETUP_REQ`/`SETUP_ACK`, as the Mac does.
- This retires `BtHandoverSetup`'s SDP and RFCOMM exchange and the stale-SDP problems with the Xiaomi.

### 6.4 What stays on classic Bluetooth

- The AirPods' own links, including `AirPodsConnector`'s RFCOMM/SDP wake probe and KS reconnect. These talk to the AirPods, not to peers.
- `TraceUploader` (debug only, on demand) until it moves to Wi-Fi or `adb`. A link opened only for an upload doesn't matter.

### 6.5 Android

- Nothing, if G3 passes as-is.
- Otherwise one change: open the GATT server with `TRANSPORT_LE` (explanation a).
- The legacy and V2 RFCOMM servers and the `BtHandoverSync` server are deleted only after the Windows LE build is in use (P4).

## 7. Fate of the quiet redesign

| Item | Status |
|---|---|
| Phases 0–3 (stats, bug fixes, events on the PCs, quiet-v1) | Done. They carry over to LE unchanged. |
| D3, D4 on Windows (events, backoff) | Done. Backoff is reused by the LE central. Dialling absent phones disappears with LE. |
| D5 Windows passive scan | Folded into §6.2 (gate S1 becomes part of G3) |
| Phase 4 `legacy-in-v2` | **Cancelled.** Superseded: LE carries legacy packets in its one link. Reopen only if option B ships. |
| Phase 5 Android scan mode (S2), D7 battery packet | Unchanged, independent, low priority |

## 8. Implementation phases

| Phase | Work | Repo | Gate to move on |
|---|---|---|---|
| **P0** | Gates G1–G6 (§5) | tool only | The decision in §5 |
| **P1** | `PeerLink` seam, C++ record codec with vectors. RFCOMM moved behind the seam, with no behaviour change. | Everypods-private | Unit tests, plus one day on the current devices with STATS unchanged |
| **P2** | `PeerGattCentral` behind a setting (default on for the test PC). RFCOMM peers off while LE is on. Watcher merged with proximity. | Everypods-private (+ the Android transport flag if G3 needs it) | Hardware matrix T1–T9 (rfcomm-removal test plan) for phone ↔ Windows. **Idle test 0 drops in 5 runs.** Overnight STATS: no reconnects while in range. |
| **P3** | Setup over LE on Windows; "Add peer" from advertisements | Everypods-private | T7 and T8 on both phones |
| **P4** | Delete the Windows RFCOMM peer code. Android deletes the legacy, V2 and `BtHandoverSync` RFCOMM servers. "Update EveryPods" message for old builds. | both | One week on LE with no fallback used |

P1 is worth doing whichever option wins: it is what lets the transport change without touching the protocol code again.

## 9. Standard idle-drop test (used by every gate and phase)

`windows/tools/bt-capture.ps1`, scenario "call":
1. Start the capture with the AirPods on the PC.
2. Make a short call, hang up, and leave the PC silent with the AirPods in your ears.
3. Wait 3 minutes, or until a drop.
4. Repeat once with media instead of a call.

Pass: no drop, and the AirPods' handle reaches sniff (Mode Change, mode 2) after the audio stops. Record each run's result and the zip name in this document.

## 10. Risks

| Risk | Mitigation |
|---|---|
| G3 fails the same way as on 22 Nov | A′ (LE-only bond) is a real path toward phones, unlike toward the Mac. B remains. |
| An LE link also disturbs the AirPods link | G2 tests this before any work. B is the fallback. |
| Windows LE links that don't drop, or `Unreachable` after a bond | `MaintainConnection`, retries, waiting for `Disconnected`, identity by installation ID |
| Phones advertising slowly while dozing make the PC relink late | Android already advertises fast for 3 min after a computer goes missing. Measure T5 for Windows. |
| A PC adapter without LE | Not seen on supported hardware. B's code path stays until P4. |

## Sources

- [Microsoft, GATT client](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-client): `MaintainConnection`, uncached discovery, the 7 s `Unreachable` timeout
- [Microsoft Q&A: retry required for GetGattServicesAsync](https://learn.microsoft.com/en-au/answers/questions/2280559/retry-required-for-getgattservicesasync()-when-con): first call on a paired LE-privacy device returns `Unreachable`
- [Nordic DevZone 48916](https://devzone.nordicsemi.com/f/nordic-q-a/48916/bluetooth-le-windows-10-using-winrt-c-code-works-if-device-not-paired-fails-with-unreachable-if-device-is-paired/194337): works unpaired, `Unreachable` once paired
- [Microsoft Q&A: multiple concurrent LE connections](https://learn.microsoft.com/en-us/answers/questions/419173/uwp-c-bluetooth-low-energy-multiple-conncurrent-co)
- [Intel community: audio drop after idle, Intel Wireless Bluetooth](https://community.intel.com/t5/Wireless/Bluetooth-Audio-Drops-After-Idle-Playback-Start-Intel-Wireless/td-p/1743279); [Intel community: v24.20.0.3 audio routing bug](https://community.intel.com/t5/Wireless/Bug-Report-Severe-A-V-Sync-Deadlock-amp-Audio-Drop-with-Intel-BT/m-p/1746451)
- Everypods `docs/rfcomm-removal-plan.md` (22 Nov spike results) and `docs/peer-ble-transport.md`
