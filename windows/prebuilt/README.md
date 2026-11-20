# Prebuilt Windows executables

Downloads-only branch: never merged. Ready-to-run copies, so they can be downloaded straight from GitHub on a
Windows PC (open the file → **Download raw file**). These are snapshots, not
built from this folder; rebuild with CMake or take a newer artifact from
**Actions → Windows CI** when the source changes.

| File | What it is | Built from |
|---|---|---|
| `Propods.exe` | Windows tray client: durable trace, **Send traces to phone**, BT handover setup, driver-level connect/disconnect, stream framing, scan pausing, and peer protocol v2 (snapshots + automatic-reconnect grants) | `feat/windows-peer-protocol-v2` @ `58cab14`, [CI run 44827770168](https://github.com/ntsour/Everypods-private/actions/runs/44827770168) |
| `ks_connect_spike.exe` | Driver-level Bluetooth audio connect/disconnect test tool (`list`, `connect`, `disconnect`, `cycle`, `watch`) | `feat/windows-ks-connect-spike` @ `0e75e54`, [CI run 44805511397](https://github.com/ntsour/Everypods-private/actions/runs/44805511397); also writes its log to the traces folder |

Both are unsigned Release x64 builds; SmartScreen may ask for "More info → Run anyway".
Traces are written to `%LOCALAPPDATA%\EveryPods\traces` (tray: **Open traces folder**).
