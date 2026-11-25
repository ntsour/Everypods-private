# Propods: peer protocol v2 (design)

Status: design, 2026-11-20. Nothing implemented yet.

## Why

The Pixel's automatic reconnect (case open / buds in) is blocked while the PC is a
handover peer: `passive acquisition blocked reason=peer_identity_missing`. Android's
passive acquisition (`CrossDevice.beginPassiveAcquisition`) proceeds only when **every**
configured peer:

1. has a v2 identity bound to its Bluetooth address (from a v2 HELLO);
2. has a ready v2 session that advertised `acquire-v1`;
3. has a fresh SNAPSHOT for the same headset with `ownership=released`, `audioUse=idle`;
4. then **grants** the ACQUIRE_REQUEST, and acknowledges ACQUIRE_START.

A grant is only valid if the participant's own view of **every member's** snapshot
revision equals the requester's (`snapshotRevisions == currentSnapshotRevisions`), and
the membership the requester lists equals the participant's configured membership. So
Propods needs v2 sessions with every configured phone, not only the requester. Phones
already do this among themselves.

Explicit handover (media start, calls) stays on the legacy 4-byte channel, unchanged.

## Wire protocol (from Android, `docs/peer-protocol-v2.md`)

- RFCOMM service `97ce0b4d-8e2f-4a6d-b6cd-d69d362e75ac`, separate socket from legacy.
- Frame: `EPH2`, version 2, type u8, payload length u32 BE (≤ 4096), UTF-8 JSON.
- Strict JSON: exact field sets, no duplicate keys, canonical lowercase UUIDs,
  non-negative integers, sorted unique lists. Any violation closes that peer's v2 link.
- Types used by Propods: HELLO 1, SNAPSHOT 2, PING 3, PONG 4, ACQUIRE_REQUEST 5,
  ACQUIRE_GRANT 6, ACQUIRE_DENY 7, ACQUIRE_CANCEL 8, ACQUIRE_START 10,
  ACQUIRE_START_ACK 11. RELAY_DATA 9 is accepted and ignored.

## Session (Propods always dials; phones never dial a computer, PR ntsour/Everypods#96)

1. Connect to the phone's v2 service.
2. Send HELLO `{installationId, sessionId, capabilities:["acquire-v1","snapshot"],
   initiatorId=installationId, connectionId=random}`.
3. Phone replies HELLO, SNAPSHOT, PING. Propods replies PONG `{nonce, snapshot}` and sends
   its own SNAPSHOT and PING; the phone's PONG completes sync. Deadline: 5 s.
4. Heartbeat: PING every 10 s; close after two missed responses. Answer every PING with
   SNAPSHOT + PONG.
5. Send a fresh SNAPSHOT whenever local state changes.

Identity: `installationId` persisted in `%APPDATA%\Propods\peer-identity.txt`;
`sessionId` new per process. `headsetId` = Android's `headsetIdFromAddress`: SHA-256 of
the uppercase `AA:BB:CC:DD:EE:FF` address, first 16 bytes, version nibble 8, variant 10.

Local SNAPSHOT `{sessionId, revision, headsetId, ownership, audioUse, lastOwnerId:null,
lastOwnerRevision:null, acquisition}`:
- `ownership`: `held` when the AirPods' classic link is up on this PC, else `released`
  (measured, not the protocol state).
- `audioUse`: `call` while the Teams mic is open (a call or meeting; a Teams sound or a
  VLC session alone is not a call), `media` while media is playing, else `idle`.
- `revision`: monotonic per session, bumped when any of the above or `acquisition` changes.
- `acquisition`: the current participant reservation, if PREPARING/IN_PROGRESS.

Per peer Propods keeps: bound installation id (from HELLO), session id, latest accepted
snapshot revision. A snapshot whose `sessionId` differs from that peer's HELLO session
is rejected (close link).

## Participant (mirrors `PassiveAcquisitionCoordinator.participant*`)

ACQUIRE_REQUEST from peer P, after checking `requesterId/requesterSessionId` match P's
HELLO:
- membership (sorted) must equal {own id} ∪ {ids of all configured peers}, and the
  SHA-256 digest must match → else DENY `MEMBERSHIP_MISMATCH`;
- `snapshotRevisions` must equal Propods' current view (own revision + each peer's
  latest) → else DENY `STALE_SNAPSHOT`;
- own session/headset unknown → DENY `UNKNOWN`; own ownership `held` or audio not
  `idle` → DENY `HELD`;
- an active reservation for a different request → DENY `BUSY` (same request → re-GRANT);
- else reserve (PREPARING, expires in 5 s), GRANT with a new `grantId`.

ACQUIRE_START: must match the reservation (request, requester, session, grantId) and
still be PREPARING and unexpired; revisions, membership and RELEASED/IDLE re-checked →
IN_PROGRESS and START_ACK (duplicate START → START_ACK again); else DENY.

ACQUIRE_CANCEL: matching reservation → CANCELLED. A HELD snapshot from the requester
resolves a PREPARING reservation.

While a reservation is PREPARING or IN_PROGRESS, Propods does **not** start an
explicit takeover of its own (the phone is mid-connect).

## Phases

| Phase | Scope | Done when |
|---|---|---|
| A | Frame codec, strict JSON, UUID/SHA-256 helpers, identity store; unit-testable without Bluetooth | Round-trips Android's encodings |
| B | v2 client per configured peer: HELLO, PING/PONG, SNAPSHOT both ways, heartbeat | Pixel logs `peer_snapshot accepted=true` for the PC; `peer_identity_missing` gone |
| C | Participant state machine, GRANT/DENY/START_ACK/CANCEL | Case-open on the Pixel reconnects the AirPods while the PC is a peer |
| D | All configured phones reachable (`pj`), reconnect/backoff, logs | Passive works with 3 members |

Setup assumption: every device must have every other configured as a BT handover peer
(the PC must also be added on `pj`), as Android already requires among phones.
