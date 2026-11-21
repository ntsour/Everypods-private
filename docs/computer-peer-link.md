# Computer peer link (Windows ↔ Windows, Windows ↔ Mac)

Phones are not needed for two computers to coordinate the AirPods. Each
computer client listens on the same RFCOMM coordination service as a phone
and dials the computers it knows. The link carries the same 4-byte packets as the phone
link (`windows/common/crossdevice_protocol.hpp`, `macos/.../Protocol.swift`),
so release and takeover work exactly as they do with a phone.

## Service

There is one coordination service, shared by phones and computers:

| | |
|---|---|
| Name | `EveryPodsCrossDevice` |
| UUID | `1abbb9a4-10e4-4000-a75c-8953c5471342` (`crossdevice::kServiceUuid`) |
| Security | Encrypted and authenticated (the devices must be paired) |

Computers behave like phones: every peer listens on this service and every
computer dials other peers on it. A phone dials a computer only when the user
picks that computer as its peer in the Android app; the computer accepts it the same way.
`PeerRegistry::Kind` (Phone/Computer) is only a label: it drives the UI, and
trace upload and the phone-only v2 mesh skip computers.

## Rules every computer client follows

1. **Listen** on the service at startup.
2. **Dial** every configured peer's service, with backoff, while no link to it
   is up (in either direction).
3. **One link per peer.** A link the peer dialled counts as the link to that
   peer: don't dial while it is up.
4. **Both dialled at once:** keep the link dialled by the **lower Bluetooth
   address**, close the other. Concretely, when a peer dials in while our own
   dialled link to it is up: if our address is lower, close the incoming link;
   otherwise close ours and use the incoming one. The higher address also waits
   3 s before its first dial, so races are rare.
5. **Unknown address dials in:** accept it and save it as a peer, a computer
   or a phone by its Bluetooth Class of Device (pairing is the trust boundary),
   so the user only adds the peer on one side. A known peer keeps its kind.
6. **Packets:** handle them as from a phone. Holder receiving
   `REQUEST_DISCONNECT` (00 02 00 00) releases the AirPods and replies
   `AIRPODS_DISCONNECTED` (00 01 00 00); a computer taking the AirPods sends
   `REQUEST_DISCONNECT` to all peers, then connects, then sends
   `AIRPODS_CONNECTED` (00 01 00 01). Non-holders answer `REQUEST_DISCONNECT`
   with `AIRPODS_DISCONNECTED`.

## Windows (feat/computer-peer-link)

- `PeerServer` advertises `kServiceUuid` and hands links to `PeerRegistry::acceptInbound`.
- `BluetoothRfcommClient` dials `kServiceUuid` for every peer and can adopt an incoming link.
- Tray: **Add peer...** lists paired devices that advertise the service,
  skips ones already configured, and asks to confirm each. Devices whose
  Bluetooth Class of Device major class is Computer are saved as `computer=`,
  everything else as `android=`. Peers show as "(phone)" or "(computer)".
  Startup auto-discovery (no peers configured) classifies the same way.
- Settings: `computer=AA:BB:...` lines in `%APPDATA%\Propods\config.txt`.
- Computer peers are not added to the v2 mesh; phones are.

## Mac (to do)

The Mac app (`macos/EveryPodsMac`, branch `feat/macos-handover-spike`) has one
`PeerLink` to one phone. To join:

- Listen on `kServiceUuid` too: publish an SDP record for `1abbb9a4-...`
  with `IOBluetoothSDPServiceRecord.publishedServiceRecord(with:)` (service
  class UUID, an RFCOMM protocol descriptor, name `EveryPodsCrossDevice`),
  and register for incoming channels with
  `IOBluetoothRFCOMMChannel.register(forChannelOpenNotifications:selector:withChannelID:direction:)`.
- Keep a list of peers (phone + computers) instead of a single `phoneAddress`;
  one `PeerLink` per peer. Dial other computers on the same
  `ServiceUUID` used for phones.
- Apply rules 3-5 above using the Mac's own address
  (`IOBluetoothHostController.default().addressAsString()`): lower address's
  link wins, higher address waits 3 s before its first dial, and an unknown
  address that dials in is added as a computer peer.
- Send every outgoing packet to all connected peers, as Windows does.

## Testing

1. Pair the two computers with each other in Bluetooth settings.
2. Run the new build on both. On one, tray → **Add peer...** and pick
   the other. Both should show the other as a connected "(computer)" peer.
3. Play audio on the computer without the AirPods: it should send
   `kRequestDisconnect`, the holder releases, and the AirPods move over.
