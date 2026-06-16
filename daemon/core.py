"""Claude Usage Tracker Daemon — shared core.

All platform-agnostic logic lives here: constants, helpers, poll_api,
_claude_state_fields, Session, connect_and_run, main. Platform specifics
(token reading, BLE discovery, single-instance lock) live in the backends.
"""
from __future__ import annotations

import asyncio
import json
import os
import re
import signal
import sys
import threading
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from watchfiles import awatch

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"  # readable; used as link-liveness probe
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

# Live Claude Code state: hooks write per-session files here; the daemon watches
# the dir and translates them into the BLE payload. See daemon/hooks/.
STATE_DIR = Path.home() / ".config" / "claude-usage-monitor" / "state"
STATE_STALE_S = 120       # a working/idle file older than this is ignored (crashed session).
DIALOG_MIN_AGE_S = 2.5    # ignore a waiting/asking file YOUNGER than this.
DIALOG_STALE_S = 600      # delete a waiting/asking file older than this.

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

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

_file_logger = None  # set by the Windows backend; None elsewhere


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass  # pythonw: stdout is None
    if _file_logger is not None:
        _file_logger.info(msg)


# ---------------------------------------------------------------------------
# Auth helpers
# ---------------------------------------------------------------------------

class AuthError(Exception):
    """Raised by poll_api on a genuine 401/403 — the token really is expired or
    invalid and the user must re-run `claude login`. Distinct from a None return,
    which means a transient failure (network/DNS, timeout, rate-limit, 5xx)."""


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
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


# ---------------------------------------------------------------------------
# API polling
# ---------------------------------------------------------------------------

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
        # The token is re-read from Keychain/credentials on every poll, so an
        # expired token self-heals the moment you re-login to Claude Code.
        log(f"API HTTP {resp.status_code}: auth failed — token likely expired; "
            f"re-login to Claude Code (the daemon picks up the new token on the next poll)")
        raise AuthError(resp.status_code)
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

    # Wall-clock for the device's clock screen.
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


# ---------------------------------------------------------------------------
# Live Claude Code state (hook state files -> BLE payload)
# ---------------------------------------------------------------------------

_waiting_seen: dict[str, tuple[float, int]] = {}
WAITING_STABLE_READS = 2
MAX_Q = 4


def _claude_state_fields() -> dict:
    """Read STATE_DIR and return the live-state payload fields.

    Returns {"cs", "aq", "q", "_queue"} where:
      cs  = 0 idle / 1 working / 2 permission prompt pending /
            3 no recent activity / 4 blocked on user question
      aq  = number of sessions currently waiting for approval
      q   = bounded list (<= MAX_Q) of {tn: tool, td: detail}, FIFO order
      _queue = list of waiting sids (internal-only; stripped before BLE send)
    """
    now = time.time()
    activity = 3          # NONE until a fresh working/idle file is seen
    activity_ts = -1.0
    asking = False
    waiting: list[tuple[float, str, str, str]] = []
    live_sids: set[str] = set()

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
                f.unlink(missing_ok=True)
                continue
            if now - ts < DIALOG_MIN_AGE_S:
                continue
            prev_ts, seen = _waiting_seen.get(sid, (0.0, 0))
            if ts != prev_ts:
                _waiting_seen[sid] = (ts, 1)
                if ts > activity_ts:
                    activity_ts = ts
                    activity = 1
                continue
            seen += 1
            _waiting_seen[sid] = (ts, seen)
            if seen < WAITING_STABLE_READS:
                if ts > activity_ts:
                    activity_ts = ts
                    activity = 1
                continue
            waiting.append((ts, sid, str(d.get("tool", ""))[:15],
                            str(d.get("detail", ""))[:60]))
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
            if ts > activity_ts or (ts == activity_ts and cand > activity):
                activity_ts = ts
                activity = cand

    for dead in [s for s in _waiting_seen if s not in live_sids]:
        del _waiting_seen[dead]

    waiting.sort(key=lambda x: x[0])
    if waiting:
        q = [{"tn": tool, "td": detail}
             for (_, _sid, tool, detail) in waiting[:MAX_Q]]
        return {
            "cs": 2,
            "aq": len(waiting),
            "q": q,
            "_queue": [w[1] for w in waiting],
        }
    if asking:
        return {"cs": 4, "aq": 1,
                "q": [{"tn": "AskUserQuestion", "td": ""}], "_queue": []}
    return {"cs": activity, "aq": 0, "q": [], "_queue": []}


# ---------------------------------------------------------------------------
# Session, _wait_first, _next_backoff  (added in Task 5)
# ---------------------------------------------------------------------------

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
        except (BleakError, ValueError, OSError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def ping(self) -> bool:
        """Probe the link with a confirmed GATT read."""
        try:
            await self.client.read_gatt_char(TX_CHAR_UUID)
            return True
        except (BleakError, asyncio.TimeoutError, EOFError) as e:
            log(f"Keepalive ping failed (link dead): {e}")
            return False

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except (BleakError, OSError) as e:
            log(f"Write failed: {e}")
            return False


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any of `events` is set, or after `timeout` seconds."""
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def _next_backoff(current: int, cap: int) -> int:
    """Double current backoff value, clamped to cap."""
    return min(current * 2, cap)


# ---------------------------------------------------------------------------
# connect_and_run  (added in Task 6)
# ---------------------------------------------------------------------------

async def connect_and_run(backend, target, stop_event: asyncio.Event,
                          tray_state=None) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    Returns True if the connection was used successfully (at least one write).
    backend.make_client(target) builds the OS-appropriate BleakClient.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = backend.make_client(target)
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

    last_usage: dict = {}
    last_pushed: str | None = None
    used = {"ok": False}

    session_stop = asyncio.Event()

    async def forward_global_stop() -> None:
        await stop_event.wait()
        session_stop.set()

    async def push() -> None:
        nonlocal last_pushed
        if not last_usage:
            return
        state = _claude_state_fields()
        state.pop("_queue", None)
        merged = {**last_usage, **state}
        blob = json.dumps(merged, separators=(",", ":"))
        if blob == last_pushed:
            return
        if await session.write_payload(merged):
            last_pushed = blob
            used["ok"] = True
            if tray_state is not None:
                tray_state.set_connected(time.time())

    async def poll_loop() -> None:
        last_poll = 0.0
        while client.is_connected and not session_stop.is_set():
            elapsed = time.time() - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = backend.read_token()
                if not token:
                    log("No token; skipping poll")
                    if tray_state is not None:
                        tray_state.set_error("token expired — run claude login")
                else:
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        if tray_state is not None:
                            tray_state.set_error("token expired — run claude login")
                        payload = None
                    if payload is not None:
                        last_usage.clear()
                        last_usage.update(payload)
                        last_poll = time.time()
                        await push()
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                if not await session.ping():
                    session_stop.set()
                    break

    async def watch_loop() -> None:
        await push()
        try:
            async for _ in awatch(STATE_DIR, stop_event=session_stop):
                if not client.is_connected or session_stop.is_set():
                    break
                await push()
        except RuntimeError:
            pass

    forwarder = asyncio.ensure_future(forward_global_stop())
    try:
        await asyncio.gather(poll_loop(), watch_loop())
    finally:
        forwarder.cancel()
        try:
            await client.disconnect()
        except (BleakError, OSError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used["ok"]


# ---------------------------------------------------------------------------
# main  (added in Task 7)
# ---------------------------------------------------------------------------

async def main(backend, tray_state=None) -> None:
    _lock = backend.single_instance()  # held for process lifetime; do not close
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    pass

    STATE_DIR.mkdir(parents=True, exist_ok=True)

    log("=== Claude Usage Tracker Daemon (BLE) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None
    while not stop_event.is_set():
        target = await backend.discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            if tray_state is not None:
                tray_state.set_scanning()
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = _next_backoff(backoff, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(backend, target, stop_event, tray_state)
        if not ok:
            skip_addr = backend.on_target_failed(addr)
            if tray_state is not None:
                tray_state.set_scanning()
            log(f"Connection lost, reconnecting in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = _next_backoff(backoff, 60)
        else:
            backoff = 1
