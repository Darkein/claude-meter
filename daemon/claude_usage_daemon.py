#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — primary cross-platform daemon (macOS/Linux).

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from watchfiles import awatch

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

# Live Claude Code state: hooks write per-session files here; the daemon watches
# the dir and translates them into the BLE payload. See daemon/hooks/.
STATE_DIR = Path.home() / ".config" / "claude-usage-monitor" / "state"
STATE_STALE_S = 120       # a working/idle file older than this is ignored (crashed session).
                          # Sized to outlast a long-running tool: no hook fires between a
                          # tool's PreToolUse (working) and PostToolUse (working at the end),
                          # so a shorter window would falsely flip the device to idle mid-run.
DIALOG_MIN_AGE_S = 2.5    # ignore a waiting/asking file YOUNGER than this. In acceptEdits mode
                          # (or with an allowlist) PermissionRequest still fires for an
                          # auto-granted tool, writing "waiting" ~instantly before PreToolUse
                          # overwrites it with "working". That transient file would otherwise
                          # flash the device's approval screen for one daemon tick. A real
                          # human-facing prompt always outlives this window; an auto-grant
                          # round-trip never does. This is the fix for the "asking screen
                          # flashes ~1s every ~30s" bug across many concurrent sessions.
DIALOG_STALE_S = 600      # delete a waiting/asking file older than this. Pressing ESC to dismiss
                          # a permission OR AskUserQuestion dialog fires NO hook (confirmed: only
                          # mid-generation ESC emits Stop), so a cancelled dialog's state file is
                          # never cleared by an event. This timeout is the only thing that clears
                          # it. Raised from 180s to 10min: now that the device can swipe between
                          # screens, a real prompt the user is reading shouldn't time out from the
                          # screen prematurely. Still bounded so an ESC-dismissed dialog clears well
                          # before the 24h hard-prune that originally caused stuck "pending" screens.

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
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
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

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
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        # The token is re-read from Keychain/credentials on every poll (see
        # poll_loop), so an expired token self-heals the moment you re-login to
        # Claude Code — no daemon restart needed. Say so, instead of a bare code.
        log(f"API HTTP {resp.status_code}: auth failed — token likely expired; "
            f"re-login to Claude Code (the daemon picks up the new token on the next poll)")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    # Wall-clock for the device's clock screen. The device has no timezone, so
    # we send *local wall time* as an epoch (UTC epoch + gmtoff): the firmware
    # stores and displays it verbatim. Lives in the 60s-poll payload (not the
    # per-change push) so it doesn't defeat the push dedup or spam BLE.
    lt = time.localtime()
    wall_epoch = int(time.mktime(lt)) + lt.tm_gmtoff

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
        "t": wall_epoch,
    }
    return payload


# --- Live Claude Code state (hook state files -> BLE payload) ---------------
#
# The daemon holds NO authoritative state in memory: the durable truth is the
# set of state/<sid>.json files the hooks write. build_state() re-derives the
# whole picture from disk each time, so a daemon restart rebuilds the identical
# queue and cannot desync. (Caveat: _waiting_seen below — the stable-read
# counter — IS in-memory only, so a restart re-incurs the WAITING_STABLE_READS
# delay for an in-flight prompt. Self-healing within a couple of reads.)


# Per-sid bookkeeping for the "is this 'waiting' a real blocked prompt, or just
# an auto-granted tool stream?" distinguisher. A genuinely-pending permission
# prompt FREEZES the agentic loop: the session writes no further state files, so
# its waiting-file ts stays put. An auto-granted stream (acceptEdits mode, or a
# long-lived session whose hook config predates the PreToolUse clearing hook)
# keeps rewriting "waiting" with an ADVANCING ts as each tool runs and nothing
# clears it. We only trust a "waiting" file as a real prompt once its ts has held
# steady across a couple of reads. Maps sid -> (waiting_ts, times_seen_unchanged).
_waiting_seen: dict[str, tuple[float, int]] = {}
WAITING_STABLE_READS = 2  # consecutive reads with an unchanged ts before we believe it
MAX_Q = 4  # max approvals sent over BLE. Bounded so the worst-case JSON fits the
           # device's 512-byte RX buffer: 4 x (tool<=15 + detail<=60) ~= 470 bytes
           # with the usage fields. The sid is NOT sent (the device never displays
           # it). aq still reports the true total for the "i / N" badge.


def _claude_state_fields() -> dict:
    """Read STATE_DIR and return the live-state payload fields.

    Returns {"cs", "aq", "q", "_queue"} where:
      cs  = 0 idle / 1 working / 2 a permission prompt is pending /
            3 no recent activity / 4 blocked on a user question (AskUserQuestion
            or MCP elicitation — needs an answer, but no allow/deny)
      aq  = number of sessions currently waiting for approval (true total)
      q   = bounded list (<= MAX_Q) of {s: sid, tn: tool, td: detail}, FIFO order,
            so the device can swipe through each pending approval
      _queue = list of waiting sids (internal-only; stripped before BLE send)

    cs=3 (NONE) when no fresh state file exists at all — distinct from cs=0
    (IDLE), which means a session just finished its turn and is awaiting input.
    Without this the device would show "Waiting for you" forever once any
    session's idle file goes stale.

    Priority when several sessions are live: permission (2) > question (4) >
    working (1) > idle (0). A permission prompt and a question both block on the
    user, so they outrank a merely-working session.
    """
    now = time.time()
    activity = 3          # NONE until a fresh working/idle file is seen
    activity_ts = -1.0
    asking = False        # any fresh session blocked on AskUserQuestion/elicitation
    waiting: list[tuple[float, str, str, str]] = []  # (ts, sid, tool, detail)
    live_sids: set[str] = set()  # sids seen this read, to prune _waiting_seen

    try:
        files = list(STATE_DIR.glob("*.json"))
    except OSError:
        files = []

    for f in files:
        try:
            d = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        st = d.get("state")
        ts = float(d.get("ts", 0))
        sid = d.get("sid") or f.stem
        if st == "waiting":
            live_sids.add(sid)
            if now - ts > DIALOG_STALE_S:
                # Past the dialog window with no hook to clear it — either an
                # ESC-dismissed dialog (no hook fires) or a truly abandoned
                # prompt. Delete it so ghosts don't accumulate on disk and can
                # never re-raise the approval screen. (Old code only skipped it
                # and kept the file for 24h, which is what left stale
                # AskUserQuestion "waiting" files lying around.)
                f.unlink(missing_ok=True)
                continue
            if now - ts < DIALOG_MIN_AGE_S:
                # Too young to trust — likely an auto-grant transient that
                # PreToolUse is about to overwrite with "working". Skip it this
                # tick; if it's a real prompt it'll still be here next tick.
                continue
            # Re-arm guard: only believe this is a real, blocked prompt once its
            # ts has held steady across WAITING_STABLE_READS reads. A session
            # that keeps advancing its waiting-ts is actively running tools
            # (auto-grant / stale-hook stream), NOT blocked on a human — count it
            # as working activity instead of raising the approval screen.
            prev_ts, seen = _waiting_seen.get(sid, (0.0, 0))
            if ts != prev_ts:
                _waiting_seen[sid] = (ts, 1)
                # ts moved since last read -> still streaming. Treat as working.
                if ts > activity_ts:
                    activity_ts = ts
                    activity = 1
                continue
            seen += 1
            _waiting_seen[sid] = (ts, seen)
            if seen < WAITING_STABLE_READS:
                # Stable for the first time but not yet long enough — treat as
                # working this tick; promote to a real prompt on the next read.
                if ts > activity_ts:
                    activity_ts = ts
                    activity = 1
                continue
            waiting.append((ts, sid, str(d.get("tool", "")) [:15],
                            str(d.get("detail", "")) [:60]))
        elif st == "asking":
            if now - ts > DIALOG_STALE_S:
                f.unlink(missing_ok=True)
                continue
            if now - ts < DIALOG_MIN_AGE_S:
                continue
            asking = True
        else:  # working / idle
            if now - ts > STATE_STALE_S:
                continue
            cand = 1 if st == "working" else 0
            # Newest ts wins; on a tie prefer working (more informative than idle).
            if ts > activity_ts or (ts == activity_ts and cand > activity):
                activity_ts = ts
                activity = cand

    # Drop bookkeeping for sids no longer in "waiting" (answered, cleared, or
    # gone) so a later prompt from the same session starts its stable-count
    # fresh rather than inheriting a stale one.
    for dead in [s for s in _waiting_seen if s not in live_sids]:
        del _waiting_seen[dead]

    waiting.sort(key=lambda x: x[0])  # FIFO: oldest first
    if waiting:
        # Send the queue (bounded) so the device can swipe between pending
        # approvals, not just the front. aq stays the true total (may exceed
        # MAX_Q -> the device shows "i / aq"). q is the bounded detail list; the
        # sid is omitted (the device doesn't display it) to save BLE bytes.
        q = [{"tn": tool, "td": detail}
             for (_, _sid, tool, detail) in waiting[:MAX_Q]]
        return {
            "cs": 2,
            "aq": len(waiting),
            "q": q,
            "_queue": [w[1] for w in waiting],
        }
    if asking:
        return {"cs": 4, "aq": 0, "q": [], "_queue": []}
    return {"cs": activity, "aq": 0, "q": [], "_queue": []}


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    # The payload sent to the device = cached usage fields (refreshed on the 60s
    # API poll) merged with live Claude-state fields (refreshed on file change).
    # Always carry the usage fields so the firmware's bars never zero out.
    last_usage: dict = {}
    last_pushed: str | None = None
    used = {"ok": False}

    async def push() -> None:
        nonlocal last_pushed
        if not last_usage:
            return  # nothing to render yet; wait for the first poll
        state = _claude_state_fields()
        state.pop("_queue", None)  # internal-only; never goes over BLE
        merged = {**last_usage, **state}
        blob = json.dumps(merged, separators=(",", ":"))
        if blob == last_pushed:
            return  # nothing changed — keep BLE quiet
        if await session.write_payload(merged):
            last_pushed = blob
            used["ok"] = True

    async def poll_loop() -> None:
        last_poll = 0.0
        while client.is_connected and not stop_event.is_set():
            elapsed = time.time() - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        last_usage.clear()
                        last_usage.update(payload)
                        last_poll = time.time()
                        await push()
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass

    async def watch_loop() -> None:
        # Push once immediately (covers state files that already exist on connect),
        # then on every change to the state dir.
        await push()
        try:
            async for _ in awatch(STATE_DIR, stop_event=stop_event):
                if not client.is_connected:
                    break
                await push()
        except RuntimeError:
            pass  # awatch can raise if the dir vanishes; poll_loop keeps us alive

    try:
        await asyncio.gather(poll_loop(), watch_loop())
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used["ok"]


def _acquire_single_instance() -> "object":
    """Refuse to start if another daemon already holds the lock.

    Two daemons fight over the one BLE device (each disconnects the other),
    which manifests as the device flickering between live data and the pairing
    screen. A whole-file flock on a lockfile is released automatically if the
    holder dies, so a crashed daemon never wedges the next start. Returns the
    open file handle, which the caller must keep alive for the process's
    lifetime (closing it drops the lock).
    """
    import fcntl

    lock_path = STATE_DIR.parent / "daemon.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    fh = open(lock_path, "w")
    try:
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log(f"Another daemon already running (lock held: {lock_path}); exiting.")
        fh.close()
        sys.exit(0)
    fh.write(str(os.getpid()))
    fh.flush()
    return fh


async def main() -> None:
    _lock = _acquire_single_instance()  # held for process lifetime; do not close
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    STATE_DIR.mkdir(parents=True, exist_ok=True)  # so awatch has a target

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
