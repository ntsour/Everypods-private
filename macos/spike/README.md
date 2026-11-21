# Bluetooth spike (macOS, IOBluetooth)

Throwaway command-line tool that checks two things from macOS:

1. Can macOS open an RFCOMM channel to the Android phone's custom service and exchange 4-byte packets?
2. Can macOS connect and disconnect a paired pair of AirPods programmatically?

## Build

```sh
cd /Users/nikos/Everypods-private/macos/spike
swiftc -O -framework IOBluetooth -framework Foundation \
  -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist \
  main.swift -o spike
```

The `-sectcreate` flags embed `Info.plist` (which holds `NSBluetoothAlwaysUsageDescription`) into the binary. Without it, macOS (TCC) aborts the process with SIGABRT the first time IOBluetooth is touched.

Requires Xcode command line tools and macOS with Bluetooth.

### Bluetooth permission

The first run needs Bluetooth access. macOS attributes the request to the *responsible* app: run the spike from Terminal.app (or iTerm) and approve the prompt there. If the process is launched from another app without a Bluetooth usage string, TCC kills it with no output, exit code 134. Check `~/Library/Logs/DiagnosticReports/spike-*.ips` for `"namespace":"TCC"` to confirm.

## Modes

```sh
./spike paired                          # list paired devices: name, MAC, isConnected (read-only)
./spike rfcomm <PHONE_MAC>              # SDP lookup, open RFCOMM channel, exchange packets
./spike airpods-connect <AIRPODS_MAC>   # openConnection, log IOReturn and isConnected
./spike airpods-disconnect <AIRPODS_MAC># closeConnection, log IOReturn and isConnected
```

MAC format: `AA:BB:CC:DD:EE:FF` (dashes also accepted).

### rfcomm

Runs an SDP query for service `1abbb9a4-10e4-4000-a75c-8953c5471342`, takes its RFCOMM channel ID, and opens it asynchronously. Logs `OPEN` on success and every received packet as hex with a timestamp.

While running, type a line on stdin to send a 4-byte packet:

| Command        | Bytes sent           |
|----------------|----------------------|
| `connected`    | `00 01 00 01`        |
| `disconnected` | `00 01 00 00`        |
| `reqdisc`      | `00 02 00 00`        |
| `reqhand`      | `00 02 00 04`        |
| `quit`         | closes the channel and exits |

Nothing is sent unless a command is typed. The channel auto-closes after 60 seconds.

What it proves: macOS can discover the custom service via SDP, open RFCOMM to it, and both send and receive small binary packets.

### airpods-connect / airpods-disconnect

`airpods-connect` calls `openConnection` with a delegate and waits for `connectionComplete:status:` (30s safety timeout). `airpods-disconnect` calls `closeConnection` and re-checks `isConnected` after one second.

What it proves: macOS can connect and disconnect AirPods from code, and whether the IOReturn codes are useful for error handling.

## Notes

- Throwaway spike. Not production code, no error recovery beyond logging.
- `paired` does not touch any device. The other three modes connect to real hardware, so only run them deliberately.

## Run as a signed .app (required)

The bare command-line binary can open channels but IOBluetooth never delivers its async
callbacks to it. Packaged as a signed app bundle, everything works (tested on the Pixel 10).

```bash
mkdir -p Spike.app/Contents/MacOS && cp Info.plist Spike.app/Contents/Info.plist
# Info.plist also needs CFBundleExecutable=spike, CFBundlePackageType=APPL, LSUIElement=true
swiftc -O -target arm64-apple-macos13.0 -framework AppKit -framework IOBluetooth -framework Foundation \
  main.swift -o Spike.app/Contents/MacOS/spike
codesign --force --sign "<Apple Development identity>" Spike.app
open -W -n --stdout /tmp/out.log --stderr /tmp/out.log Spike.app --args setup-client <PHONE_MAC>
```

Use `-target ...macos13.0`: the default deployment target is newer than the OS and `open` fails with -10825.

## Modes added after the first spike
- `setup-client <PHONE_MAC>`: authorizes this Mac on the Pixel via its EveryPodsBTHandoverSetup service
  (two exchanges: send revision 1, then echo the Pixel's state back). Run once after adding the Mac in the Android app.
- `rfcomm <PHONE_MAC>`: opens the EveryPodsCrossDevice channel and logs the packets (works once authorized).
- `setup-server`: hosts the setup service on the Mac; the Pixel could not reach it (L2CAP config timeout). Not needed.
