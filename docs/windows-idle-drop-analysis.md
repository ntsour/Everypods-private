# Windows idle AirPods drop: analysis and conclusions

Status: analysis complete, 2026-11-28. The fix being pursued is to move the Windows peer links to LE (`windows-peer-links-le-plan.md`). The fallback is the classic option 1 below, if LE fails its checks.

## Symptom

With EveryPods running, the AirPods drop from the Windows PC 30–60 s after audio stops, typically after a handover from a phone ("play briefly, pause, leave idle"). Plain Windows Bluetooth never drops.

Hardware:
- Lenovo 20X2, Intel Wireless Bluetooth driver 24.20.0.3, Windows build 26200
- AirPods Pro 3 (20:52:1D:89:33:C9)
- Pixel 10 (30:E0:44:86:61:2C)
- Xiaomi Pad 7 Pro "pj" (68:4D:B6:A3:8D:7E)

## How it was traced

- HCI captures with Microsoft's Bluetooth trace profile (`windows/tools/bt-capture.ps1`: WPR + BTP `btetlparse`, read with tshark), together with the EveryPods Windows traces.
- The quiet redesign (#31–#35, Everypods #123–#125) came first. It cut idle traffic by 15–20× but did not stop the drop, which ruled out "too much traffic" as the cause.

## Captures

| When | Setup | Result |
|---|---|---|
| 27 Nov 20:15 | EveryPods on, both phones linked, handover from the Pixel (call), idle | Lost (0x08) 58 s after the call |
| 27 Nov 20:23 | EveryPods closed | No drop. The AirPods asked for sniff 4 s after the call. |
| 27 Nov 20:32 | EveryPods on, phones' Bluetooth off | No drop. Sniff after 40 s. |
| 27 Nov ~20:40 | EveryPods on, phones on, no capture | AirPods left 3 s after connecting, several times. Probably taken back, not an idle loss. Not evidence either way. |
| 28 Nov 09:31 / 09:36 / 09:42 (G1) | Links already up, AirPods connected from Settings: both phones / Pixel only / Xiaomi only | No drop in any. Sniff after 38–40 s. |
| 28 Nov 09:57 | Handover from the Pixel (media), capture started before | No drop. Sniff 40 s after the A2DP suspend. |
| 28 Nov 10:02 | Same | **Lost (0x08)** |
| 28 Nov 10:04 | Same | **Lost (0x08)** |

## Mechanism

**What every lost link had in common** (silent from = Disconnect Complete − the 20 s supervision timeout):

| Run | Audio stopped | Xiaomi link brought out of sniff | AirPods link silent from | Lost |
|---|---|---|---|---|
| 27 Nov | SCO closed 20:16:18.6 | 20:16:49.4–56.0 | ≈ 20:16:56.9 | 20:17:16.9 |
| 28 Nov 10:02 | A2DP suspend 10:02:07.8 | 10:02:35.73 | ≈ 10:02:35.8 | 10:02:55.8 |
| 28 Nov 10:04 | A2DP suspend 10:04:11.1 | 10:04:37.08 | ≈ 10:04:39.9 | 10:04:59.9 |

**The surviving run (09:57):**
- The Xiaomi link was woken at 09:57:32.7, while A2DP was still streaming, and at 09:58:33.6, after the AirPods had entered sniff (09:58:24).
- It was never woken inside the idle window.

**The sequence:**
1. The PC dials the AirPods itself during the handover (`KS ONESHOT_RECONNECT`). It is central on that link, and Windows writes a link policy with role switch **off** and sniff on.
2. After the audio stops, the AirPods link stays in **active mode with no traffic** for about 40 s, until the AirPods request sniff. Windows never sends Sniff_Mode for this link itself.
3. If the **Xiaomi** classic link leaves sniff inside that window, the AirPods link goes silent within ≤ 3 s. 20 s later the controller reports a connection timeout (0x08). On 27 Nov an Authenticated Payload Timeout event came first.
   - Windows takes a link out of sniff to send RFCOMM data: it writes the link policy with sniff off, then the Mode Change to active follows.
4. Waking the **Pixel** link at the same moments (10:02:28, 10:04:29) did no harm.
5. On 27 Nov the PC was **peripheral** on the Xiaomi link: the Xiaomi took the central role through a Role Change at link setup. Most likely reading: while the Intel controller follows the Xiaomi's piconet in active mode, it stops polling its own idle piconet with the AirPods. That is a scatternet scheduling weakness in this controller or driver. The roles for the 28 Nov links weren't captured, because the links predated the captures.

**What woke the Xiaomi link** in all three losses: EveryPods' quiet-v1 idle probe (`V2 send PING`). It goes out after 60 s without frames. That is about 60 s after the handover messages, which is 30–40 s after a short play, so it lands right in the window. Before quiet-v1, traffic every 10 s hit the window even more reliably.

**Why it looked random:** the trigger has to land inside a roughly 40 s window that only exists after audio stops on a link the PC just dialled. The G1 runs had no handover and no traffic in the window, so they never dropped.

**Not explained:** with EveryPods running, the AirPods take about 40 s to request sniff after audio stops. With EveryPods closed (27 Nov 20:23), it took 4 s. A shorter window would make the drop rarer. Worth checking if we stay on classic.

## What doesn't cause it

| Ruled out | Evidence |
|---|---|
| EveryPods' audio handling (routing, KS reconnect, media hold) | No drop with the phones' Bluetooth off, though EveryPods ran the same audio logic |
| The amount of peer traffic | The quiet redesign cut it 15–20×; one message in the window is enough |
| Any extra classic ACL | 27 Nov 20:23 had another ACL in sniff and no drop; waking the Pixel link is harmless |
| How the AirPods link is created | All 28 Nov handover runs created it identically; one survived |

## Options if we stay on classic

| # | Option | Assessment |
|---|---|---|
| **1** | **Quiet while this PC holds the AirPods.** No idle probes, battery packets, keepalives or snapshot re-sends on phone links while this PC holds the AirPods. Real events still go through: handover requests, ownership changes, acquisition. Needs both Windows and Android, since phones probe and send battery packets too. | **The fallback.** Small. Removes the trigger seen in every loss. Leaves the weakness itself: anything else that wakes the Xiaomi link in the window can still drop the AirPods. Test: the handover scenario, 0 drops in 10. |
| 2 | Close the phone links while the PC holds the AirPods; redial after they leave | Removes the cause, but no coordination while the PC holds them: a phone takes the AirPods unasked, and a call on the PC isn't protected. Closing must happen while audio is still streaming, never inside the window. |
| 3 | Keep the PC central on phone links | Not possible: no Windows or Android app API controls the role |
| 4 | Keep the AirPods link busy (stream silence) | Drains the AirPods' battery. Rejected. |
| 5 | Newer Intel driver | Worth one test (G6). It doesn't help other users' PCs. |

## Decision (2026-11-28)

- **Move the Windows peer links to Bluetooth LE** (`windows-peer-links-le-plan.md`). An LE link has no sniff transitions or piconet roles of this kind. LE also fixes a second classic problem: Windows can't tell when an absent phone is back, because phones never dial computers. On 28 Nov the Xiaomi stayed unlinked until Propods was restarted, because the dialler was in a 2+ minute backoff.
- LE must pass G2 first: an idle LE link with traffic every 2 s (`le_spike hold --ping 2`) must not drop the AirPods in the handover scenario.
- **If LE fails** (G2 drops, or G3 and G4 can't reach the GATT service), implement **option 1**.

## Artifacts

- Captures and traces: the uploads of 27–28 Nov (`bt-capture-*.zip`), copied to the session scratchpad. Not kept in the repo.
- Tools: `windows/tools/bt-capture.ps1` (HCI capture), `windows/tools/le_spike.cpp` and `windows/tools/le-checks.ps1` (LE checks).
- Useful tshark filters: Mode Change `bthci_evt.code==0x14`, Role Change `0x12`, Disconnect Complete `0x05`, Write Link Policy `bthci_cmd.opcode==0x080d` (fields `bthci_cmd.link_policy_switch`, `bthci_cmd.link_policy_sniff`), Create Connection `0x0405` (`bthci_cmd.allow_role_switch`), AVDTP Start/Suspend on the AirPods handle (`btavdtp`).
