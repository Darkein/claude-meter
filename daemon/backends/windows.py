"""Windows platform backend for the Claude Usage Daemon.

Reads the token from credential files (LOCALAPPDATA / APPDATA / home),
discovers via BleakScanner scan-by-name (no cached MAC), uses BleakClient
with address_type="random" and use_cached_services=False (WinRT NimBLE
requirements), and provides a no-op single_instance (the tray already
holds the named mutex via _acquire_single_instance in tray_windows.py).
"""
import datetime
import json
import logging
import logging.handlers
import os
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

from daemon import core
from daemon.backends.base import Backend


def _build_file_logger() -> "logging.Logger | None":
    """Create a rotating file logger for field diagnostics, or None.

    Autostart launches the tray under pythonw.exe, which has no console — stdout
    is discarded (and is in fact None, making print() unsafe). A rotating file is
    then the ONLY trail when the daemon stalls in the field. Windows-only: on the
    Linux dev box / CI the console print() suffices, and gating to win32 keeps the
    pure-helper unit tests from writing stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("claude-meter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import (tray imports this module)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    path = base / "claude-meter" / "daemon.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort — logging setup must never stop the daemon
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S")
    )
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


def _windows_credential_candidates() -> list:
    """Return the ordered list of credential file paths to probe (first hit wins).

    Priority:
    1. CLAUDE_CREDENTIALS_PATH env override (project-specific)
    2. CLAUDE_CONFIG_DIR env override (official Claude override)
    3. Candidate list: home/.claude, LOCALAPPDATA/Claude, APPDATA/Claude
    """
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override)]
    if config_dir := os.environ.get("CLAUDE_CONFIG_DIR"):
        return [Path(config_dir) / ".credentials.json"]
    home = Path.home()
    local_appdata = Path(
        os.environ.get("LOCALAPPDATA", home / "AppData" / "Local")
    )
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    return [
        home / ".claude" / ".credentials.json",          # primary
        local_appdata / "Claude" / ".credentials.json",  # fallback 2
        appdata / "Claude" / ".credentials.json",        # fallback 3
    ]


def _read_expiry() -> str:
    """Return human-readable expiry from the first-hit credentials file."""
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            data = json.loads(raw)
            oauth = data.get("claudeAiOauth", {})
            expires_ms = oauth.get("expiresAt")
            if expires_ms is None:
                return "expiry unknown"
            dt = datetime.datetime.fromtimestamp(
                expires_ms / 1000, tz=datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%d %H:%M UTC")
        except (TypeError, ValueError, OSError, AttributeError, json.JSONDecodeError):
            return "expiry unknown"
    return "expiry unknown"


def read_token() -> str | None:
    """Module-level convenience: read the Claude OAuth access token.
    Delegates to WindowsBackend logic without requiring an instance."""
    for path in _windows_credential_candidates():
        try:
            return core._extract_access_token(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return None


class WindowsBackend(Backend):
    name = "windows"

    def __init__(self) -> None:
        # Inject the file logger into core so log() writes to disk under pythonw.
        logger = _build_file_logger()
        if logger is not None:
            core._file_logger = logger

    def read_token(self) -> str | None:
        """Read the Claude OAuth access token from the first available credential file."""
        for path in _windows_credential_candidates():
            try:
                return core._extract_access_token(path.read_text(encoding="utf-8"))
            except OSError:
                continue
        return None

    async def discover_target(self, skip_addr=None):
        core.log(f"Scanning for '{core.DEVICE_NAME}' ({core.SCAN_TIMEOUT}s)...")
        device = await BleakScanner.find_device_by_name(
            core.DEVICE_NAME, timeout=core.SCAN_TIMEOUT
        )
        if device:
            core.log(f"Found: {device.address}")
        return device  # BLEDevice or None

    def make_client(self, target):
        return BleakClient(
            target,
            address_type="random",
            use_cached_services=False,
        )

    def on_target_failed(self, addr):
        return None  # no address cache to drop on Windows

    def single_instance(self) -> object:
        """No-op: the tray already holds the named mutex via
        tray_windows._acquire_single_instance. Return a sentinel."""
        return object()
