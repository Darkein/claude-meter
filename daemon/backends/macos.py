"""macOS platform backend for the Claude Usage Daemon.

Reads the token from the macOS Keychain, discovers via CoreBluetooth
retrieveConnectedPeripherals (handles HID-grabbed devices invisible to
scans) with scan fallback, uses a plain BleakClient (no extra kwargs),
and holds a process-wide fcntl flock for single-instance.

CoreBluetooth internals are imported LAZILY inside methods — this module
is importable on Linux (for test collection) without requiring macOS.
"""
import fcntl
import getpass
import os
import subprocess
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

from daemon import core
from daemon.backends.base import Backend

KEYCHAIN_SERVICE = "Claude Code-credentials"


class MacOSBackend(Backend):
    name = "macos"
    _cb_manager = None

    def read_token(self) -> str | None:
        try:
            out = subprocess.run(
                [
                    "security",
                    "find-generic-password",
                    "-s",
                    KEYCHAIN_SERVICE,
                    "-a",
                    getpass.getuser(),
                    "-w",
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.CalledProcessError as e:
            core.log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
            return None
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            core.log(f"Keychain access error: {e}")
            return None
        return core._extract_access_token(out.stdout)

    async def _get_cb_manager(self):
        """Lazily create and ready a shared CoreBluetooth central manager."""
        if MacOSBackend._cb_manager is None:
            from bleak.backends.corebluetooth.CentralManagerDelegate import (
                CentralManagerDelegate,
            )

            mgr = CentralManagerDelegate()
            await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
            MacOSBackend._cb_manager = mgr
        return MacOSBackend._cb_manager

    async def _retrieve_connected(self, skip_addr=None):
        """Return a BLEDevice for a system-connected Clawdmeter, or None.

        Two-step lookup, strongest signal first:

        1. Peripherals connected under our CUSTOM service UUID. Membership in
           that service is unambiguous (no other device exposes it), so we accept
           by service alone — the peripheral's name can be None on macOS.
        2. Fall back to the generic HID service 0x1812, but ONLY trust a
           peripheral whose name matches DEVICE_NAME. 0x1812 also matches
           unrelated keyboards/mice, so picking blindly here could grab the
           wrong device.

        ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
        stale CoreBluetooth handle can't trap us into never trying a fresh scan.
        """
        from CoreBluetooth import CBUUID
        from bleak.backends.device import BLEDevice

        try:
            manager = await self._get_cb_manager()
        except Exception as e:  # BleakBluetoothNotAvailableError etc.
            core.log(f"CoreBluetooth unavailable: {e}")
            return None

        cm = manager.central_manager

        def _wrap(p):
            addr = p.identifier().UUIDString()
            core.log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
            return BLEDevice(addr, p.name(), (p, manager))

        def _ok(p) -> bool:
            return not (skip_addr and p.identifier().UUIDString() == skip_addr)

        # 1. Custom service — accept by service membership alone.
        custom = cm.retrieveConnectedPeripheralsWithServices_(
            [CBUUID.UUIDWithString_(core.SERVICE_UUID)]
        )
        for p in custom or []:
            if _ok(p):
                return _wrap(p)

        # 2. Generic HID service — require an exact name match.
        hid = cm.retrieveConnectedPeripheralsWithServices_(
            [CBUUID.UUIDWithString_("1812")]
        )
        for p in hid or []:
            if _ok(p) and p.name() == core.DEVICE_NAME:
                return _wrap(p)

        return None

    async def discover_target(self, skip_addr=None):
        dev = await self._retrieve_connected(skip_addr=skip_addr)
        if dev is not None:
            return dev
        core.log(
            f"Not held by OS; scanning for '{core.DEVICE_NAME}' ({core.SCAN_TIMEOUT}s)..."
        )
        dev = await BleakScanner.find_device_by_name(
            core.DEVICE_NAME, timeout=core.SCAN_TIMEOUT
        )
        if dev:
            core.log(f"Found: {dev.address}")
        return dev

    def make_client(self, target):
        return BleakClient(target)

    def on_target_failed(self, addr):
        return addr  # skip this stale handle next cycle (no string cache on macOS)

    def single_instance(self) -> object:
        lock_path = core.STATE_DIR.parent / "daemon.lock"
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        fh = open(lock_path, "w")
        try:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            core.log(
                f"Another daemon already running (lock held: {lock_path}); exiting."
            )
            fh.close()
            sys.exit(0)
        fh.write(str(os.getpid()))
        fh.flush()
        return fh
