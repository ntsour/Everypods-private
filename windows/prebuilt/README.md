# Prebuilt Windows executables

Ready-to-run copies, so they can be downloaded straight from GitHub on a
Windows PC (open the file → **Download raw file**). These are snapshots, not
built from this folder; rebuild with CMake or take a newer artifact from
**Actions → Windows CI** when the source changes.

| File | What it is | Built from |
|---|---|---|
| `Propods.exe` | Windows tray client with the durable trace and **Send traces to phone** | `feat/windows-handover-trace` @ `61b6220`, [CI run 44795748692](https://github.com/ntsour/Everypods-private/actions/runs/44795748692) |
| `ks_connect_spike.exe` | Driver-level Bluetooth audio connect/disconnect test tool (`list`, `connect`, `disconnect`, `cycle`, `watch`) | `feat/windows-ks-connect-spike` @ `28ae3af`, [CI run 44777269597](https://github.com/ntsour/Everypods-private/actions/runs/44777269597) |

Both are unsigned Release x64 builds; SmartScreen may ask for "More info → Run anyway".
Traces are written to `%LOCALAPPDATA%\EveryPods\traces` (tray: **Open traces folder**).
