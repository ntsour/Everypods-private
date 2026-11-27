# Quiet peer links: an event-driven redesign

Status: proposal, 2026-11-27. Nothing here is implemented.
- It amends `peer-mesh-spec.md` §1.3 (liveness) and §2 (freshness), both in the Everypods repo.
- It keeps the transports chosen in `rfcomm-removal-plan.md`: Windows ↔ phones over RFCOMM, Mac ↔ phones over LE, and no Mac ↔ Windows link.
- It doesn't change the handover rules or the acquisition handshake.

## Why

When nothing is happening, the peers still check on each other and re-send their state every few seconds. Below is what the Windows traces from 21–26 Nov show. "Code" means read from the source, not measured.

| Finding | Evidence |
|---|---|
| The PC and two phones exchange 3,500–5,000 messages an hour while idle | All traces, overnight included |
| Each phone wakes every 10 s just to answer the PC | Night of 25–26 Nov (13.7 h, AirPods not on the PC): each phone answered ~4,400 pings, median reply 200–260 ms |
| Phones close healthy legacy links 45–48 s after they come up | 128 times that night; the Pad about 12/h on 26 Nov afternoon (bug B1) |
| The PC dials an absent phone forever | 26 Nov 14:20–15:42: ~370 failed dials of ~10 s each, so the radio spent about three quarters of that time dialling |
| Pixel links drop and come back | Up to 9/h; both channels at once, Pixel back ~3 s later; cause unknown |
| Windows looks up the AirPods' state ~1.5 times a second | Code: 1 s handover watcher plus the 2 s V2 monitor, ~130,000 lookups a day |
| Windows wakes its threads ~30 times a second while idle | Code: 100 ms sleep slices |
| The Mac pings each phone every 2 s over LE, on top of the V2 and legacy heartbeats | Code: `PeerGattCentral.pingInterval = 2` |

The root cause is a design choice, not just timers:
- Android treats a peer's state as valid for only **35 s** after it arrives (`PeerOwnershipStore.freshnessMs`).
- So every link has to re-send its state about every 10 s, through the PONG, or lid-open acquisitions get refused.
- On top of that, every layer runs its own liveness check: the V2 PING from each side, the legacy status ping, and the LE transport ping on the Mac.

## Goals

| Metric (idle, phones in range) | Today | Target |
|---|---|---|
| Messages per phone–PC pair | 1,750–2,500/h | ≤ 120/h |
| Messages per phone–Mac pair | ~4,500/h (code) | ≤ 120/h |
| Phone wake-ups caused by one peer | every 10 s | ≤ 1/min |
| Link reconnects per hour | 2–12 (plus B1) | only real link loss |
| Dials to an unreachable peer | ~270/h | ≤ 15/h after 5 min |
| Windows AirPods state lookups | ~5,400/h | on events, plus one per minute |
| Windows idle thread wake-ups | ~30/s | ≤ 1/s |
| Time for a newly connected peer to learn who holds the AirPods | up to 20 s or more | < 1 s |
| Handover and lid-open latency | — | unchanged (measured before and after) |

## Principles

1. **The transport reports dead links.** A dead link shows up as a socket error, end of stream, `ConnectionStatusChanged`, `ACTION_ACL_DISCONNECTED` or CoreBluetooth `didDisconnect`. The Bluetooth supervision timeout bounds this at about 20 s. App-level probes exist only to catch a hung peer app, and run only after a silence.
2. **State is pushed when it changes, and stays valid for its session.** Safety comes from the grant handshake (snapshot revisions plus an explicit grant within 2 s), not from re-sending state.
3. **Ask when it matters.** An action that needs fresh state sends one refresh, which already exists as `CrossDevice.refreshPassiveOwnership`, instead of every peer refreshing all the time.
4. **Local state comes from OS events.** Windows already does this for media and calls. The AirPods' connection state moves to events too.
5. **Dial only when it can work:** at start, when the peer's link comes up, right after a drop, or on a backoff that grows to minutes.
6. **One link per pair** (spec §1.2) **and one liveness check per link.**
7. **Never wait by polling.** Block on events, condition variables or timers.

## Design

### D1. Liveness (replaces spec §1.3)

- Each session records when it last received any frame.
- **Idle probe:**
  - After `T_idle` = 60 s with nothing received, send a PING.
  - Close the link if 2 PINGs in a row, 15 s apart, go unanswered.
  - A hung peer app is then found within about 90 s. A lost link is still found within about 20 s, by the transport.
- Both sides run the same probe, and any frame resets it. A PING resets the receiver's timer and its PONG resets the sender's, so in practice one exchange a minute keeps a quiet pair alive.
- PING is answered at once by the link code, never by the handover logic (as on the LE transport today).
- The handshake is unchanged: HELLO / SNAPSHOT / PING / PONG, complete within 5 s.
- **Legacy status keepalive** (phone → PC, every 20 s): stops toward a peer whose V2 session is ready and advertises `quiet-v1` (see Compatibility). The V2 session proves that the same app on the same device is alive. Toward older peers it stays, with B1 fixed.
- **LE (Mac ↔ phones):** the 2 s transport ping is replaced by the idle probe. A phone app restarting under a live LE link is handled by events instead:
  - `didModifyServices`, as today
  - a write error, which triggers the same rediscovery
  - the idle probe, as a last resort
- `T_idle` is a single constant per client. Start at 60 s. Go to 120 s once link lifetimes are confirmed stable (see Risks).

### D2. State and freshness (amends spec §2)

- Snapshots are still pushed on every change. The PONG keeps carrying the snapshot, for compatibility and as a free re-sync on every probe.
- Answering a PING with both a SNAPSHOT frame and a PONG carrying the same snapshot stops: Windows sends only the PONG.
- **Validity:**
  - An observation from a `quiet-v1` peer is valid while its session is ready and its link is up.
  - The 35 s window stays for peers without `quiet-v1`.
  - This changes `PeerOwnershipStore` on Android and the equivalent on Mac. Windows has no time-based freshness.
- **Acquisition safety is unchanged.** `ACQUIRE_*` carries snapshot revisions, the holder denies `STALE_SNAPSHOT` on a mismatch, and grants must arrive within 2 s. A peer whose app hung can't grant, so nothing unsafe can happen.
- **Lid-open:** the coordinator starts on session-valid snapshots. If it gets `STALE_SNAPSHOT`, it refreshes and retries within the existing `PeerRefreshBudget`.
- **New links:** each side sends its full state immediately. For V2 this happens already. For the legacy link, Windows sends `AIRPODS_CONNECTED`/`AIRPODS_DISCONNECTED` to the peer that connected (B2).

### D3. Local state from events

**Windows**
- Look up the AirPods' `BluetoothDevice` once, and again only when the configured address changes. Subscribe to `ConnectionStatusChanged`. On each change, run the existing edge handling in `HandoverController`, then call `PeerV2Mesh::refreshLocalState()`.
- Keep one reconcile a minute as a safety net. It logs any edge the event missed, which tells us whether the event can be trusted.
- Remove the 1 s watcher thread and the 2 s `refreshLocalState()` in `PeerV2Mesh::monitorLoop`. The monitor then only serves heartbeat deadlines, waiting with `wait_until` the next deadline.
- Replace every 100 ms sleep slice with a condition-variable wait, as `BluetoothRfcommClient` already does:
  - the `runPeer` linked wait
  - the hands-free check
  - the call-mic debounce
  - `sleepWhile`

**Mac**
- Replace the 1 s `pollBluetooth` with IOBluetooth connect and disconnect notifications for the AirPods, keeping a 60 s reconcile.
- `MediaWatcher`: the Core Audio listeners already drive it. Run the 1 s tick only while a start is being confirmed.
- `PeerV2Hub`: schedule the next deadline instead of ticking every second.
- `PeerGattCentral`: query `retrieveConnectedPeripherals` only while unlinked, as `peer-ble-transport.md` says.

**Android**
- Peer link-up is already event-driven (`ACL_CONNECTED` → `retryNow`).
- The keepalive changes are in D1, and scanning is in D5.

### D4. Dialling (Windows first; the same rules on Android phone ↔ phone)

- **Dial when:**
  - at start
  - when the peer's link comes up (already wired)
  - once, about 3 s after a drop: the Pixel re-links in ~3 s
  - when the user asks
  - on the backoff timer
- **Backoff for a peer that doesn't answer:** 2, 4, 8, 15, 30, 60, 120, then 300 s, with ±20 % jitter. Reset when the peer's link comes up.
- The separate, longer backoff for a reachable device with no EveryPods service stays as it is (`peerServiceMissingDelay`).
- **One page per dial to an absent peer:**
  - When the peer's link is down, connect with the cached service record first.
  - Run the uncached lookup only if the device answered but the channel was wrong (stale SDP, as seen with the Xiaomi).
  - Today an absent peer costs two pages per dial: the uncached lookup, then the connect.
- **Don't dial on a timer while this PC holds the AirPods and audio is active.** Event-driven dials still happen. Android already has a 15 s floor for this (`MIN_DELAY_DURING_AUDIO_MS`); `peer-link.md` notes that Windows lacks it.

### D5. Scanning

- **Windows:** switch to a passive scan with an `AdvertisementFilter` on Apple's company ID (0x004C).
  - The proximity message is in the advertisement itself, so the active scan requests (sent to every advertiser nearby) shouldn't be needed. Gate S1 below checks this.
  - Keep pausing while this PC holds the AirPods, and stop while Windows sleeps.
- **Android:** `BLEManager` runs `SCAN_MODE_LOW_LATENCY` with a hardware filter around the clock. Measure lid-open detection with `SCAN_MODE_BALANCED`, and switch if latency stays acceptable (gate S2). This is separate from the PC work.
- **Mac:** it already stops scanning in sleep. No change.

### D6. One link per RFCOMM pair

- LE already carries the legacy packets inside its one link (record `0x02 LEGACY`). RFCOMM pairs, Windows ↔ phone and phone ↔ phone, still open two links.
- Add a V2 frame type `LEGACY` that carries one 4-byte packet, gated on the capability `legacy-in-v2`.
- When both ends advertise it:
  - the dialler stops dialling the legacy service
  - the legacy packets travel in V2 frames
  - the legacy listener stays only for older builds
- The unified handshake (`acquire-v2`, spec §4) later replaces the legacy packets entirely. `legacy-in-v2` halves the links, dials and lookups now, without waiting for it.

### D7. Packets nobody reads

- Android sends `PeerBatteryPacket` (`00 05 00 01 …`) to Windows, which ignores it ("Unknown packet (8 bytes)", 13–147 an hour).
- Send it only to peers that advertise a capability for it (`battery-v1`), or move it into the snapshot if a peer ever shows it.

### Bugs fixed in phase 1 (no protocol change)

| # | Where | Bug | Fix |
|---|---|---|---|
| B1 | Android `CrossDevice.kt`, server keepalive (`SERVER_KEEPALIVE_INTERVAL_MS`, `LegacyPeerLiveness`) | It checks expiry right after sending the request, before the reply can arrive. The wait (coroutine `delay`) pauses during device sleep, but the expiry uses `elapsedRealtime`, which keeps counting. | Check before sending the next request, never right after. The link is stale only when an earlier request is still unanswered after 2 rounds and at least 45 s. |
| B2 | Windows `HandoverController::onPeerConnectionChanged` | A newly connected legacy peer isn't told the PC's state; it waits for its next status request | Send the state to that peer. This needs the peer address in the callback. |
| B3 | Windows `HandoverController::onIncomingPacket`, `RequestConnectionStatus` | The reply goes to every peer | Reply to the sender only. This needs the source address. |
| B4 | Windows `PeerV2Mesh::handleFrame`, PING | Answers with SNAPSHOT plus a PONG carrying the same snapshot | Send the PONG only |

## Compatibility

Two new HELLO capabilities:
- `quiet-v1`: "I push every state change, my snapshots are valid for the whole session, I answer PING at once, and I run the idle probe."
- `legacy-in-v2`: "Send me legacy packets as V2 `LEGACY` frames; don't open the legacy service to me."

| Pair | Behaviour |
|---|---|
| Both `quiet-v1` | Idle probe at `T_idle`, session-scoped validity, no legacy keepalive |
| New ↔ old | The new side keeps today's 10 s PING and 35 s window toward the old peer, and answers its PINGs. Messages to old peers don't drop, but nothing breaks. |
| New Windows ↔ old Android | The Android peer's 10 s PINGs keep Windows' idle probe from ever firing. B1–B4 and D3–D5 apply regardless. |
| Both `legacy-in-v2` | One RFCOMM link; otherwise both links, as today |

Prerequisite: Windows and current Android `main` already ignore unknown capability names, but the **Mac still rejects them** (`PeerV2.swift`: every capability must be in its own list). That fix ships first, before any client advertises `quiet-v1`. Any phone still running a build from before Android ignored unknown names has to be updated as well.

## Phases

Each phase ships on its own and has a gate measured from the traces.

| Phase | Work | Protocol change | Gate |
|---|---|---|---|
| 0. Stats | Each client logs one line per peer per hour: frames and bytes in and out, dials, failed dials, reconnects, link lifetimes, probes; on Windows also AirPods state lookups | none | Lines present on all three clients |
| 1. Bugs | B1 (Android), B2–B4 (Windows) | none | Overnight: no legacy close 45–48 s after connect; with phones in range, reconnects only where both channels drop together |
| 2. PC events | D3 Windows and Mac, D4 Windows, D5 Windows (after gate S1) | none | Windows lookups ≈ 1/min, wake-ups ≤ 1/s, dials to an absent phone ≤ 15/h after 5 min; handover scenarios unchanged |
| 3. Quiet links | Mac capability tolerance, then `quiet-v1` on all three: D1, D2 | capability | Messages per pair ≤ 120/h; phone wake-ups ≤ 1/min per peer; lid-open success rate and latency unchanged; no rise in `STALE_SNAPSHOT` denials |
| 4. One RFCOMM link | D6 `legacy-in-v2` (Windows and Android) | capability, new frame type | One RFCOMM link per phone; dials per reconnect halved |
| 5. Phones | D5 Android (after gate S2), D7 | capability for D7 | Lid-open detection latency unchanged |

The `acquire-v2` work in `peer-mesh-spec.md` §10 continues on top of phase 4.

## Link with the Windows idle drop

The phases double as experiments for the drop. Run the 41 s repro ("Connect AirPods now", play nothing) after each one:
- **After phase 2**, EveryPods' own polling on the PC is gone (theory 2).
- **After phase 3**, idle traffic on the phone links falls about 15–20 times (theory 1).
- If the drops continue with quiet links, neither theory holds and the HCI capture decides.

The tests already proposed (no peers, then Pixel only, then Pad only) can run now on the current build, and should, since they need no code.

## Gates and spikes

- **S1, Windows passive scan:** run active and passive scans 10 min each with the AirPods out of the case, and compare how many proximity messages arrive and whether the lid-open bit appears. Go passive only if nothing is lost.
- **S2, Android scan mode:** measure lid opening to `lid_open` in `LidAutoconnectDiagnostics`, 10 opens each in `LOW_LATENCY` and `BALANCED`.
- **S3, quiet link lifetime:** before phase 3 ships, run one build with `T_idle` = 60 s for a day and compare link lifetimes with today's.
  - `CrossDevice.kt` says Android Doze drops an idle link. The traces show links dropping even with traffic every 10 s, so this is unproven.
  - If quiet links die sooner, set `T_idle` just under the observed limit, not back to 10 s.

## Risks and open questions

1. **`ConnectionStatusChanged` for the AirPods.** Windows may miss an edge for a classic audio device. The one-minute reconcile logs every miss. If misses happen, find out why before re-adding polling.
2. **A missed state push lasts for the whole session.** The PONG on every idle probe re-syncs the state within `T_idle`, and acquisitions are protected by revisions.
3. **A hung peer app is found more slowly** (~90 s instead of ~20 s). Actions need a grant within 2 s anyway: a hung holder fails the grant, and a `media` request connects anyway after 2 s (spec decision 3).
4. **Lid-open without the 35 s window.** If session validity ever lets a stale state through, the holder's revision check denies it and the retry refreshes. Phase 3's gate measures this.
5. **Old builds.** The compatibility table keeps old peers working, at today's traffic level.

## Not changing

- The handover and acquisition rules, the 2 s, 3 s and 5 s grant timers, and the 2 s unreachable-holder rule (spec §4 and decision 3)
- Media and call detection on Windows (already event-driven)
- The transport per pair (`rfcomm-removal-plan.md`)
