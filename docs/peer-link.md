# Peer link: every EveryPods app is an equal peer

There is no phone/computer distinction in the protocol. Android, Windows and
Mac run the same link rules; the code never branches on the kind of device.
The rules come from `docs/peer-mesh-spec.md` section 1 (links) and follow
Android, the reference implementation (`CrossDevice.kt`,
`CrossDeviceClient.kt`, `utils/peer/PeerConnectionArbitration.kt`).

## Services

| Service | UUID | Used for |
|---|---|---|
| Legacy coordination (`EveryPodsCrossDevice`) | `1abbb9a4-10e4-4000-a75c-8953c5471342` (`crossdevice::kServiceUuid`) | 4-byte handover packets (`windows/common/crossdevice_protocol.hpp`) |
| Peer protocol v2 (`EveryPodsCrossDeviceV2`) | `97ce0b4d-8e2f-4a6d-b6cd-d69d362e75ac` | EPH2 frames: HELLO, SNAPSHOT, PING/PONG, ACQUIRE_* |

Both are RFCOMM, encrypted and authenticated (the devices must be paired).
Until the unified acquire handshake ships (spec section 8) the handover
packets still travel on the legacy link, so every pair needs both links.

## Rules every peer follows

1. **Listen** on the legacy and the v2 service at startup.
2. **Dial** every configured peer it has no link with, on legacy and on v2,
   with backoff (2, 4, 8, then 15 s; see "Dial and retry"). A link the peer dialled counts as
   the link to that peer: don't dial that service while it is up.
3. **First dial order.** The peer with the smaller `installationId` dials at
   once; the other waits 3 s before its first dial, then dials anyway. A peer
   whose `installationId` is not known yet dials at once. The wait is only a
   head start: it makes races rare, and because the waiting side still dials,
   a link forms even when the other side never dials (a peer added on one side
   only, an older build, Android, which does not dial computers).
4. **Learning the peer's id.** The peer's `installationId` comes from its v2
   HELLO and is remembered per Bluetooth address (Windows:
   `%APPDATA%\Propods\peer-ids.txt`; Android: `PeerIdentityStore.bindPeerAddress`).
   Our own id is the persistent `installationId` in `peer-identity.txt`.
5. **Duplicate v2 links.** If two v2 links to the same peer come up, both ends
   keep the one with the smallest `(initiatorId, connectionId)` from HELLO and
   close the others (`PeerConnectionArbitration.preferred`,
   `CrossDevice.registerV2Connection`; Windows `arbitrateLocked`). The
   Bluetooth address is not used.
6. **Duplicate legacy links.** If a link we dialled and a link the peer dialled
   are both up: when both `installationId`s are known keep the link dialled by
   the peer with the smaller id and close the other; otherwise keep the link
   that came up first and close the newer. (Windows `PeerRegistry::acceptInbound`
   and `BluetoothRfcommClient::runLoop`, using `v2::chooseLegacyLink`.)
7. **Unknown address dials in:** accept it, save it as a peer (pairing is the
   trust boundary) and add it to the v2 mesh, so the user only adds the peer
   on one side. v2 links from an address that is not a configured peer are
   closed, as Android does.
8. **v2 and the legacy link are independent.** Each side dials v2 by rules 2-3;
   which side dialled the legacy link does not matter. Duplicates are settled
   by the HELLO keys.

## v2 accepting side

The side that accepts a v2 link answers like Android's v2 server
(`CrossDevice.handleV2ClientConnection`):

- It waits for the dialler's HELLO. `initiatorId` must equal the sender's
  `installationId`; the sender's own id must not be ours or already bound to
  another configured peer.
- It answers HELLO with its own `installationId`, `sessionId`, capabilities
  and the dialler's `initiatorId` and `connectionId` echoed unchanged, then
  SNAPSHOT and PING.
- The dialler answers SNAPSHOT + PING; PONG to our PING makes the session
  ready. PING every 10 s, close after two missed responses, handshake must
  finish within 5 s. Any validation failure closes the link.

The dialling side checks that the answering HELLO echoes its own
`initiatorId` and `connectionId`.

## Dial and retry

Windows dials exactly like Android, so a restarted peer relinks quickly. Android
references are in `android/app/src/main/java/io/automated/ventures/everypods/utils/`.

1. **Resolve fresh on every dial.** Never reuse a cached channel. Windows asks
   for the service with `BluetoothCacheMode::Uncached` and falls back to the
   cached query once if that fails or finds nothing (the log says which was
   used). Android creates a new socket per dial (`CrossDeviceClient.kt`,
   `createSocket`).
2. **8 s connect timeout**, then cancel the connect and close the socket
   (`CrossDeviceClient.kt` `CONNECT_TIMEOUT_MS`, `PeerConnectDeadline`). Windows
   applies the same 8 s to the device lookup and the service lookup.
3. **Backoff 2, 4, 8, then 15 s**, repeating 15 s, each times a random factor
   in 0.8..1.2 (`PeerConnectionState.kt` `PeerRetryPolicy.delaysMs`;
   `CrossDeviceClient.kt` `Random.nextDouble(0.8, 1.2000001)`). The attempt
   counter resets to 0 when a link comes up. Windows does not apply
   Android's 15 s floor while local audio is active
   (`MIN_DELAY_DURING_AUDIO_MS`); the pure function supports it but nothing
   passes the audio state yet.
4. **retryNow.** When the peer's Bluetooth link comes up (`ACL_CONNECTED` on
   Android, `BluetoothDevice::ConnectionStatusChanged` to `Connected` on
   Windows), cancel the backoff wait and dial at once, if there is no link
   (`CrossDeviceClient.kt` `retryNow`). It does not cut the 3 s first-dial wait
   of rule 3 above.
5. **One dial at a time** across all peers and both services
   (`PeerConnectionSupervisor.kt` `connectSerialized`). Windows holds one
   process-wide mutex around resolve plus connect.
6. **No baseband or radio reset** on the link path.

Windows code: `PeerDialPolicy.hpp` (timeout, schedule, jitter; covered by
`peer_v2_tests`), `dialRfcomm()` in `BluetoothRfcommClient.cpp` (steps 1, 2, 5,
shared by the legacy and v2 dial loops), `BluetoothRfcommClient::retryNow` and
`PeerV2Mesh::retryNow` (step 4, triggered from `PeerRegistry`).

## Audio-session packets

`00 03 00 01` / `00 03 00 00` (`kPeerAudioActive` / `kPeerAudioIdle`) tell the
receiver that the sender has an active call/meeting on the AirPods. Any peer
may send them. The receiver does not start its own call takeover while the
flag is set (Android `CrossDevice.peerAudioActive`; Windows
`HandoverController`). The flag is cleared by the idle packet or when the last
link drops.

Windows sends `kPeerAudioActive` while Teams has its mic open (any Teams capture
session Active, reported by `TeamsCallWatcher`) and the AirPods are on the PC,
and `kPeerAudioIdle` when the mic stays closed for 3 s or the AirPods leave. A
peer that connects mid-call gets `kPeerAudioActive` again. This stops a phone
call ringing on a phone from taking the AirPods out of a Teams call.

## Windows

- `PeerServer` advertises the legacy service and hands links to
  `PeerRegistry::acceptInbound`; `PeerV2Mesh` advertises the v2 service and
  accepts v2 links (`onInboundSocket`).
- `PeerRegistry` / `BluetoothRfcommClient` dial the legacy service for every
  peer; `PeerV2Mesh::runPeer` dials v2 for every peer.
- `v2/PeerV2Arbitration.hpp` holds the pure rules (first-dial delay, v2 key
  order, legacy duplicate choice) and is covered by `peer_v2_tests`.
- Tray: **Add peer...** lists paired devices that advertise the service and
  asks to confirm each. Peers are listed by name, with no kind label.
- Settings: one peer list, written as `android=AA:BB:...` lines in
  `%APPDATA%\Propods\config.txt`. The key name is historical (older builds
  read it); `computer=` and `peer=` lines are read too.
- Trace upload goes to every peer. It uses its own RFCOMM service, so a peer
  that does not offer it just reports that.

## Mac (to do)

The Mac app (`macos/EveryPodsMac`) follows the same rules; nothing is special
about it:

- Listen on both services (publish SDP records with
  `IOBluetoothSDPServiceRecord.publishedServiceRecord(with:)` and register for
  incoming channels with
  `IOBluetoothRFCOMMChannel.register(forChannelOpenNotifications:...)`).
- Keep one link per configured peer instead of a single peer address.
- Use the persistent `installationId` for the first-dial order and for
  rules 5-6; learn each peer's id from its v2 HELLO.
- The Mac's own Bluetooth name (not needed for any rule, but handy for UI)
  is `IOBluetoothHostController.default().nameAsString()`.
- Send every outgoing legacy packet to all connected peers.

## Testing

1. Pair the two devices in Bluetooth settings.
2. Run the build on both. On one, add the other (tray **Add peer...**). Both
   should show the other as connected; the other side saves it automatically
   when it dials in.
3. Check the trace: the peer with the larger id logs "waiting 3000 ms before
   the first dial", and a duplicate link is logged once and closed.
4. Play audio on the device without the AirPods: it sends
   `REQUEST_DISCONNECT`, the holder releases, the AirPods move over.
