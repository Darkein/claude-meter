"""Linux platform backend for the Claude Usage Daemon.

Reads the token from ~/.claude/.credentials.json, discovers by cached MAC
address or BleakScanner scan-by-name, uses a plain BleakClient (no extra
kwargs), and holds a process-wide fcntl flock for single-instance.
"""
import fcntl
import os
import re
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

from daemon import core
from daemon.backends.base import Backend


class LinuxBackend(Backend):
    name = "linux"

    def __init__(self) -> None:
        self._addr_file = Path.home() / ".config" / "claude-meter" / "ble-address"

    def read_token(self) -> str | None:
        try:
            raw = (Path.home() / ".claude" / ".credentials.json").read_text()
        except OSError as e:
            core.log(f"Error reading credentials: {e}")
            return None
        return core._extract_access_token(raw)

    def _load_cached_address(self) -> str | None:
        if not self._addr_file.exists():
            return None
        addr = self._addr_file.read_text().strip()
        if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
            r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
        ):
            return addr
        core.log("Cached address malformed, discarding")
        self._addr_file.unlink(missing_ok=True)
        return None

    def _save_address(self, addr: str) -> None:
        self._addr_file.parent.mkdir(parents=True, exist_ok=True)
        self._addr_file.write_text(addr)

    async def discover_target(self, skip_addr=None):
        address = self._load_cached_address()
        if not address:
            core.log(f"Scanning for '{core.DEVICE_NAME}' ({core.SCAN_TIMEOUT}s)...")
            dev = await BleakScanner.find_device_by_name(
                core.DEVICE_NAME, timeout=core.SCAN_TIMEOUT
            )
            if dev:
                core.log(f"Found: {dev.address}")
                self._save_address(dev.address)
                address = dev.address
        return address

    def make_client(self, target):
        return BleakClient(target)

    def on_target_failed(self, addr):
        core.log("Invalidating cached address")
        self._addr_file.unlink(missing_ok=True)
        return None

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
