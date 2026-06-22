"""One-shot BLE OTA firmware push.

Streams a firmware .bin to a connected Claude Meter over the OTA characteristic,
then exits. Reuses the platform backend's discovery + client creation so it
behaves identically to the main daemon's connect path.

Wire protocol (see firmware/src/ble.cpp OtaCallbacks): each GATT write is one
frame = 1-byte opcode + payload.
  0x01 BEGIN: u32 LE total size + 32-byte SHA-256 of the whole image
  0x02 DATA : raw image bytes, in order
  0x03 END  : finalize (device verifies SHA-256, commits, reboots)
  0x04 ABORT: cancel

The device reports the terminal result on the TX characteristic as
{"ota":"done"} or {"ota":"err","c":<code>}.

IMPORTANT: a running daemon holds the single-instance lock and a BLE connection
slot, so stop it first (launchctl/systemctl) before pushing. This command does
not take the lock.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
from pathlib import Path

from bleak.exc import BleakError

from daemon.core import (
    INFO_CHAR_UUID,
    OTA_CHAR_UUID,
    TX_CHAR_UUID,
    log,
)

# Frame opcodes.
_OP_BEGIN = 0x01
_OP_DATA = 0x02
_OP_END = 0x03
_OP_ABORT = 0x04

END_FRAME = bytes([_OP_END])
ABORT_FRAME = bytes([_OP_ABORT])

# How long to wait for the device's final {"ota":...} status after END.
FINAL_STATUS_TIMEOUT = 60.0

# DATA frames go write-without-response for speed; every Nth is a confirmed
# write to bound the OS's (macOS CoreBluetooth) outbound queue. The device side
# can no longer be overrun — its BLE task buffers into a RAM ring drained by the
# main loop, blocking (link-layer backpressure) rather than dropping — so this
# now only paces the host queue and can run looser than before.
BARRIER_EVERY = 48


def build_begin(size: int, sha256: bytes) -> bytes:
    """BEGIN frame: opcode + u32 LE size + 32-byte digest."""
    if len(sha256) != 32:
        raise ValueError("sha256 must be 32 bytes")
    return bytes([_OP_BEGIN]) + size.to_bytes(4, "little") + sha256


def build_data(chunk: bytes) -> bytes:
    """DATA frame: opcode + raw image bytes."""
    return bytes([_OP_DATA]) + chunk


def chunk_image(image: bytes, payload_size: int) -> list[bytes]:
    """Split image into payloads of at most payload_size bytes (no opcode)."""
    if payload_size < 1:
        raise ValueError("payload_size must be >= 1")
    return [image[i:i + payload_size] for i in range(0, len(image), payload_size)]


def _payload_size(mtu: int) -> int:
    """Usable image bytes per write: MTU minus 3 (ATT header) minus 1 (opcode)."""
    return max(20, (mtu or 23) - 3 - 1)


async def run(backend, bin_path: str, expected_board: str | None = None) -> int:
    """Push bin_path to a discovered device. Returns a process exit code."""
    path = Path(bin_path)
    if not path.is_file():
        log(f"Firmware not found: {path}")
        return 2
    image = path.read_bytes()
    if not image:
        log("Firmware file is empty")
        return 2
    digest = hashlib.sha256(image).digest()
    log(f"Firmware: {path} ({len(image)} bytes, sha256={digest.hex()[:16]}…)")

    target = await backend.discover_target()
    if not target:
        log("Device not found")
        return 2

    client = backend.make_client(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return 2
    if not client.is_connected:
        log("Connection failed (not connected)")
        return 2
    log("Connected")

    try:
        # --- Board guard: refuse a wrong-board image. ---
        try:
            info = json.loads(bytes(await client.read_gatt_char(INFO_CHAR_UUID)).decode())
            log(f"Device: board={info.get('board')} fw={info.get('fw')} git={info.get('git')}")
            dev_board = info.get("board")
            if expected_board and dev_board and expected_board != dev_board:
                log(f"Board mismatch: device is '{dev_board}', --board is "
                    f"'{expected_board}'. Aborting.")
                return 2
        except (BleakError, ValueError, UnicodeDecodeError) as e:
            # Old firmware without INFO_CHAR -> can't verify. Refuse unless the
            # operator omitted --board (i.e. explicitly accepted the risk).
            if expected_board:
                log(f"Cannot read device info ({e}); --board given, aborting.")
                return 2
            log(f"Cannot read device info ({e}); proceeding without board guard.")

        # --- Subscribe to terminal status. ---
        done = asyncio.Event()
        result: dict = {"ok": False, "err": None}

        def on_tx(_char, data: bytearray) -> None:
            try:
                msg = json.loads(bytes(data).decode())
            except (ValueError, UnicodeDecodeError):
                return
            if msg.get("ota") == "done":
                result["ok"] = True
                done.set()
            elif msg.get("ota") == "err":
                result["err"] = msg.get("c")
                done.set()

        await client.start_notify(TX_CHAR_UUID, on_tx)

        payload_size = _payload_size(getattr(client, "mtu_size", 0))
        log(f"MTU={getattr(client, 'mtu_size', '?')} -> {payload_size} B/chunk")

        # --- BEGIN. The device erases the OTA partition synchronously here, so
        # this write can take a couple of seconds before its response. ---
        await client.write_gatt_char(OTA_CHAR_UUID, build_begin(len(image), digest),
                                     response=True)

        # --- DATA. response=True paces the stream against the device's flash
        # writes (built-in flow control). ---
        chunks = chunk_image(image, payload_size)
        total = len(image)
        sent = 0
        next_log = 10
        for idx, chunk in enumerate(chunks):
            if done.is_set():  # device errored mid-stream
                break
            # Write-without-response for throughput (many packets per connection
            # event); every BARRIER_EVERY-th is confirmed to bound the OS queue
            # and pace the stream. A dropped chunk fails safe via the device's
            # end-to-end SHA-256 check (it rejects and keeps current firmware).
            confirmed = (idx + 1) % BARRIER_EVERY == 0
            await client.write_gatt_char(OTA_CHAR_UUID, build_data(chunk),
                                         response=confirmed)
            sent += len(chunk)
            pct = sent * 100 // total
            if pct >= next_log:
                log(f"  {pct}% ({sent}/{total})")
                next_log = pct - (pct % 10) + 10

        if result["err"] is not None:
            log(f"Device aborted mid-transfer, code={result['err']}")
            return 1

        # --- END -> device verifies + commits + reboots. ---
        await client.write_gatt_char(OTA_CHAR_UUID, END_FRAME, response=True)

        try:
            await asyncio.wait_for(done.wait(), timeout=FINAL_STATUS_TIMEOUT)
        except asyncio.TimeoutError:
            log("No final status (device may have rebooted before notifying).")
            return 0

        if result["ok"]:
            log("OTA complete — device is rebooting into the new firmware.")
            return 0
        log(f"OTA failed on device, error code={result['err']}.")
        return 1
    finally:
        try:
            await client.disconnect()
        except (BleakError, OSError):
            pass
