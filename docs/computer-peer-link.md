# Computer peer link (Windows ↔ Windows, Windows ↔ Mac)

Phones are not needed for two computers to coordinate the AirPods. Each
computer client listens on a **computer peer** RFCOMM service and dials the
computers it knows. The link carries the same 4-byte packets as the phone
link (`windows/common/crossdevice_protocol.hpp`, `macos/.../Protocol.swift`),
so release and takeover work exactly as they do with a phone.

## Service

| | |
|---|---|
| Name | `EveryPodsComputerPeer` |
| UUID | `fc163292-cca9-4217-a075-81566bca4af3` |
| Security | Encrypted and authenticated (the computers must be paired) |

Phones never advertise or dial this service. It is separate from the phone's
`EveryPodsCrossDevice` service (`1abbb9a4-…`) so a computer is never taken for
a phone.

## Rules every computer client follows

1. **Listen** on the computer peer service at startup.
2. **Dial** every configured computer peer's computer peer service, with
   backoff, while no link to it is up (in either direction).
3. **One link per peer.** A link the peer dialled counts as the link to that
   peer: don't dial while it is up.
4. **Both dialled at once:** keep the link dialled by the **lower Bluetooth
   address**, close the other. Concretely, when a peer dials in while our own
   dialled link to it is up: if our address is lower, close the incoming link;
   otherwise close ours and use the incoming one. The higher address also waits
   3 s before its first dial, so races are rare.
5. **Unknown computer dials in:** accept it and save it as a peer (pairing is
   the trust boundary), so the user only adds the peer on one side.
6. **Packets:** handle them as from a phone. Holder receiving
   `REQUEST_DISCONNECT` (00 02 00 00) releases the AirPods and replies
   `AIRPODS_DISCONNECTED` (00 01 00 00); a computer taking the AirPods sends
   `REQUEST_DISCONNECT` to all peers, then connects, then sends
   `AIRPODS_CONNECTED` (00 01 00 01). Non-holders answer `REQUEST_DISCONNECT`
   with `AIRPODS_DISCONNECTED`.

## Windows (feat/computer-peer-link)

- `ComputerPeerServer` advertises the service and hands links to `PeerRegistry::acceptInbound`.
- `BluetoothRfcommClient` dials either service and can adopt an incoming link.
- Tray: **Add computer peer (Windows/Mac)…** lists paired devices that
  advertise the service; peers show as "(phone)" or "(computer)".
- Settings: `computer=AA:BB:…` lines in `%APPDATA%\Propods\config.txt`.

## Mac (to do)

The Mac app (`macos/EveryPodsMac`, branch `feat/macos-handover-spike`) has one
`PeerLink` to one phone. To join:

- Publish an SDP record for the service with
  `IOBluetoothSDPServiceRecord.publishedServiceRecord(with:)` (service class
  UUID above, an RFCOMM protocol descriptor, name `EveryPodsComputerPeer`),
  and register for incoming channels with
  `IOBluetoothRFCOMMChannel.register(forChannelOpenNotifications:selector:withChannelID:direction:)`.
- Keep a list of peers (phone + computers) instead of a single `phoneAddress`;
  one `PeerLink` per peer, dialling `ServiceUUID.computerPeer` for computers.
- Apply rules 3–5 above using the Mac's own address
  (`IOBluetoothHostController.default().addressAsString()`).
- Send every outgoing packet to all connected peers, as Windows does.

## Testing

1. Pair the two computers with each other in Bluetooth settings.
2. Run the new build on both. On one, tray → **Add computer peer…** and pick
   the other. Both should show the other as a connected "(computer)" peer.
3. Play audio on the computer without the AirPods: it should send
   `kRequestDisconnect`, the holder releases, and the AirPods move over.
