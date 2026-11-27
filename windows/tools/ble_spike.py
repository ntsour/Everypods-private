"""SPIKES S1/S2 (not for release): can Windows use the EveryPods GATT service as a central?

S1: the phone's GATT server (Pixel / Xiaomi, running EveryPods with peers configured).
S2: the Mac's spike peripheral (macos/spike-peripheral, started with `open SpikePeripheral.app`).

The question is security: the phone/Mac characteristics require encryption. Windows is bonded with them over
classic Bluetooth only. Does Windows derive LE keys from that bond (no prompt), prompt to pair, or fail?

Setup on the PC (once):   py -m pip install bleak==0.22.3
Run:                      py ble_spike.py            (scan, then test every device advertising the service)
                          py ble_spike.py --pair     (if the plain run fails with an authentication/encryption
                                                      error: ask Windows to pair over LE first, then retry)
Paste the whole output back.
"""
import argparse
import asyncio
import json
import sys
import time
import uuid

from bleak import BleakClient, BleakScanner

SERVICE = "78fc4c78-0bdc-435d-b4a8-7d8b5f0c0a04"
RX = "78fc4c78-0bdc-435d-b4a8-7d8b5f0c0a05"
TX = "78fc4c78-0bdc-435d-b4a8-7d8b5f0c0a06"


def hello_record() -> bytes:
    payload = json.dumps({"v": 1, "id": str(uuid.uuid4()), "name": "WindowsSpike", "cap": ["legacy1"]}).encode()
    return bytes([0x01]) + len(payload).to_bytes(2, "big") + payload


async def test(device, adv, pair: bool) -> bool:
    t0 = time.monotonic()
    ms = lambda: f"{(time.monotonic() - t0) * 1000:.0f} ms"
    print(f"\n=== {device.name or '?'}  {device.address}  rssi={adv.rssi}")
    try:
        async with BleakClient(device, timeout=20.0, winrt={"use_cached_services": False}) as client:
            print(f"connected at {ms()}, mtu={client.mtu_size}")
            if pair:
                try:
                    ok = await client.pair(protection_level=2)  # 2 = Encryption
                    print(f"pair(Encryption) -> {ok} at {ms()}")
                except Exception as e:  # noqa: BLE001
                    print(f"pair failed: {e!r}")
            print("services: " + ", ".join(str(s.uuid)[:8] for s in client.services))
            svc = client.services.get_service(SERVICE)
            print(f"service found: {svc is not None}")
            if svc is None:
                return True
            tx = svc.get_characteristic(TX)
            if "read" in tx.properties:  # the Mac spike's TX is readable with encryption required
                try:
                    value = await client.read_gatt_char(TX)
                    print(f"encrypted READ ok: {value!r} at {ms()}")
                except Exception as e:  # noqa: BLE001
                    print(f"encrypted READ FAILED: {e!r}")
            received = []
            try:
                await client.start_notify(TX, lambda _, data: received.append(bytes(data)))
                print(f"SUBSCRIBE (encrypted CCCD) ok at {ms()}")
            except Exception as e:  # noqa: BLE001
                print(f"SUBSCRIBE FAILED: {e!r}")
            try:
                await client.write_gatt_char(RX, hello_record(), response=False)
                print(f"WRITE HELLO (encrypted) sent at {ms()}")
            except Exception as e:  # noqa: BLE001
                print(f"WRITE FAILED: {e!r}")
            await asyncio.sleep(4)
            print(f"notifications received: {[r[:16].hex() for r in received]}")
            return True
    except Exception as e:  # noqa: BLE001
        print(f"CONNECT FAILED at {ms()}: {e!r}")
        return False


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pair", action="store_true", help="pair over LE (encryption) before using the service")
    parser.add_argument("--scan", type=float, default=10.0, help="scan seconds")
    parser.add_argument("--skip", default="", help="comma-separated address prefixes to skip, e.g. 56:8B")
    parser.add_argument("--tries", type=int, default=3, help="connect attempts per device")
    args = parser.parse_args()
    print(f"python {sys.version.split()[0]}, scanning {args.scan:.0f} s for the EveryPods service...")
    found = await BleakScanner.discover(timeout=args.scan, service_uuids=[SERVICE], return_adv=True)
    if not found:
        print("nothing advertises the service: is the phone app running / the Mac spike open?")
        return
    skip = [p.strip().upper() for p in args.skip.split(",") if p.strip()]
    for device, adv in found.values():
        if any(device.address.upper().startswith(p) for p in skip):
            continue
        for attempt in range(1, args.tries + 1):
            print(f"--- attempt {attempt}/{args.tries}")
            if await test(device, adv, args.pair):
                break
            await asyncio.sleep(2)


asyncio.run(main())
