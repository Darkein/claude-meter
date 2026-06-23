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

DEVICE_NAME = "Claude Meter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"  # readable; used as link-liveness probe
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
OTA_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"   # firmware frames written here
INFO_CHAR_UUID = "4c41555a-4465-7669-6365-000000000006"  # board id + fw version (read)

POLL_INTERVAL = 60
TICK = 5
FAST_TICK = 0.4   # poll-loop re-check spacing while a dialog is still maturing
                  # (young or not yet stability-confirmed). A permission prompt
                  # fires ONE file write, so awatch wakes push() once; without a
                  # fast re-check the stability gate's reads would be one slow
                  # TICK apart and a prompt would surface ~5-10s late. Re-checking
                  # fast collapses that to the DIALOG_MIN_AGE_S floor (~3s).
SCAN_TIMEOUT = 8.0

# Live Claude Code state: hooks write per-session files here; the daemon watches
# the dir and translates them into the BLE payload. See daemon/hooks/.
STATE_DIR = Path.home() / ".config" / "claude-meter" / "state"
STATE_STALE_S = 120       # an activity value (working/idle) older than this is ignored (crashed session).
DIALOG_MIN_AGE_S = 2.5    # a "waiting" dialog must be this old before it counts (debounce auto-grants).
DIALOG_STALE_S = 45       # a dialog (waiting/asking) older than this is ignored. No hook fires on
                          # ESC-abandon, so this short TTL is the only event-free clear; a new prompt
                          # clears it instantly via "working".

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

# Error payloads carry ok=False plus ec (machine code) + em (short, device-
# displayable message). The device shows `em` verbatim on an error screen; the
# daemon keeps the last good usage numbers underneath so they reappear on
# recovery. Keep em short — it shares the 512-byte BLE RX buffer.
EM_MAX = 90


def _error_payload(ec: str, em: str) -> dict:
    return {"ok": False, "ec": ec, "em": em[:EM_MAX]}


def _api_error_message(resp) -> str:
    """Best-effort human message from an Anthropic error body, else raw text."""
    try:
        m = resp.json().get("error", {}).get("message")
        if isinstance(m, str) and m.strip():
            return m.strip()
    except (ValueError, AttributeError):
        pass
    return (resp.text or "").strip()


async def poll_api(token: str) -> dict:
    """Poll the API and return a payload dict — usage (ok=True) on success, or an
    error overlay (ok=False, ec, em) on a catchable failure. Raises AuthError on
    a genuine 401/403 so the caller can drive the 'run claude login' tray toast."""
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return _error_payload("network", "No connection to\nAnthropic API")
    if resp.status_code in (401, 403):
        # The token is re-read from Keychain/credentials on every poll, so an
        # expired token self-heals the moment you re-login to Claude Code.
        log(f"API HTTP {resp.status_code}: auth failed — token likely expired; "
            f"re-login to Claude Code (the daemon picks up the new token on the next poll)")
        raise AuthError(resp.status_code)
    # A 429 means the account hit a usage limit — but the response still
    # carries the anthropic-ratelimit-unified-* headers (the rejected window
    # reports ~100% plus a -reset countdown), so we parse it like a 200 below
    # instead of dropping the update. Only genuinely unusable statuses bail.
    rate_limited = resp.status_code == 429
    if resp.status_code >= 400 and not rate_limited:
        code = resp.status_code
        log(f"API HTTP {code}: {resp.text[:200]}")
        if code == 529:
            return _error_payload("overloaded", "Anthropic API overloaded\nRetrying…")
        if code >= 500:
            return _error_payload("server", f"Anthropic API error {code}\nRetrying…")
        # Other 4xx (400/404/413/...) aren't self-explanatory — surface the raw
        # API message so the user sees exactly what went wrong.
        msg = _api_error_message(resp)
        return _error_payload("api", f"API error {code}\n{msg}" if msg else f"API error {code}")

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

    s = pct(hdr("anthropic-ratelimit-unified-5h-utilization"))
    w = pct(hdr("anthropic-ratelimit-unified-7d-utilization"))
    st = hdr("anthropic-ratelimit-unified-5h-status", "unknown")

    if rate_limited:
        # The account is provably saturated. The 429 usually still includes
        # the unified headers, but if a header is absent (some 429s omit them)
        # pin the 5h window to 100% so the device shows the cap, not a stale 0.
        if s == 0:
            s = 100
        if st == "unknown":
            st = "rejected"
        log(f"API HTTP 429: rate-limited — reporting 5h={s}% 7d={w}%")

    payload = {
        "s": s,
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": w,
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": st,
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
MAX_SESSIONS = 6   # max rows in the multi-session dashboard list (ss)


# Map an "asking" dialog kind to the tool name the firmware keys its wording on
# (AskUserQuestion -> "Claude is asking", ExitPlanMode -> "Approve plan?").
_ASK_KIND_TO_TOOL = {"plan": "ExitPlanMode", "question": "AskUserQuestion",
                     "elicitation": "AskUserQuestion"}


def _claude_state_fields() -> dict:
    """Read STATE_DIR and return the live-state payload fields.

    Each per-session file carries two independent facts (see daemon/hooks/):
      activity : "working" | "idle"      — turn progress (with activity_ts)
      dialog   : "none" | "waiting" | "asking"  — pending UI (with dialog_ts);
                 takes priority over activity so a turn-pause idle can never
                 hide an open dialog.

    Returns {"cs", "aq", "q", "_queue"} where:
      cs  = 0 idle / 1 working / 2 permission prompt pending /
            3 no recent activity / 4 blocked on user question
      aq  = number of sessions currently waiting for approval
      q   = bounded list (<= MAX_Q) of {tn: tool, td: detail}, FIFO order
      _queue = list of waiting sids (internal-only; stripped before BLE send)
    """
    now = time.time()
    any_working = False   # any fresh session is making progress
    any_idle = False      # any fresh session is paused/finished
    asking_kind: str | None = None   # kind of the most-recent fresh "asking"
    asking_ts = -1.0
    asking_name = ""                 # project folder of that "asking" session
    waiting: list[tuple[float, str, str, str, str]] = []
    live_waiting_sids: set[str] = set()
    # Per-session records for the dashboard list (ss). One per fresh session;
    # the per-session state is resolved after the loop (reusing the gated
    # waiting set so a transient auto-grant doesn't flash a session as waiting).
    session_recs: list[tuple[str, str, str, bool, str | None, bool, float]] = []
    maturing = False      # a fresh waiting dialog exists but isn't confirmed yet
                          # (too young, or awaiting its 2nd stable read) -> the
                          # poll loop should re-check fast, not on the slow TICK.

    try:
        files = list(STATE_DIR.glob("*.json"))
    except OSError:
        files = []

    for f in files:
        try:
            d = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        sid = d.get("sid") or f.stem
        dialog = d.get("dialog", "none")
        dialog_ts = float(d.get("dialog_ts", 0))
        act = d.get("activity")
        act_ts = float(d.get("activity_ts", 0))

        dialog_fresh = dialog in ("waiting", "asking") and now - dialog_ts <= DIALOG_STALE_S
        activity_fresh = act in ("working", "idle") and now - act_ts <= STATE_STALE_S
        if not dialog_fresh and not activity_fresh:
            # Nothing fresh left (finished long ago / crashed before SessionEnd,
            # or a pre-upgrade single-field file). Drop it so the dir stays clean.
            f.unlink(missing_ok=True)
            continue

        # Record this live session for the dashboard list (state resolved below).
        rec_ts = max(dialog_ts if dialog_fresh else 0.0,
                     act_ts if activity_fresh else 0.0)
        session_recs.append((sid, str(d.get("name", ""))[:20], dialog, dialog_fresh,
                             act, activity_fresh, rec_ts))

        # --- dialog (waiting/asking) takes priority over activity ---
        if dialog == "waiting" and now - dialog_ts <= DIALOG_STALE_S:
            live_waiting_sids.add(sid)
            if now - dialog_ts >= DIALOG_MIN_AGE_S:
                # Stability gate: a real blocked prompt holds a steady dialog_ts
                # across reads; an acceptEdits auto-grant stream keeps advancing
                # it and so never reaches the threshold (treated as working via
                # its activity field).
                prev_ts, seen = _waiting_seen.get(sid, (0.0, 0))
                if dialog_ts != prev_ts:
                    _waiting_seen[sid] = (dialog_ts, 1)
                    maturing = True   # first sighting; needs one more stable read
                else:
                    seen += 1
                    _waiting_seen[sid] = (dialog_ts, seen)
                    if seen >= WAITING_STABLE_READS:
                        waiting.append((dialog_ts, sid,
                                        str(d.get("tool", ""))[:15],
                                        str(d.get("detail", ""))[:60],
                                        str(d.get("name", ""))[:20]))
                    else:
                        maturing = True
            else:
                maturing = True       # too young to count yet -> re-check soon
        elif dialog == "asking" and now - dialog_ts <= DIALOG_STALE_S:
            if dialog_ts > asking_ts:
                asking_ts = dialog_ts
                asking_kind = str(d.get("kind") or "question")
                asking_name = str(d.get("name", ""))[:20]

        # --- activity (fallback when no dialog wins) ---
        # Working dominates idle across concurrent sessions: the aggregate is
        # "working" if ANY fresh session is working, and only "idle" once they
        # have ALL gone idle. (Previously the most-recent activity_ts won, so a
        # session ending with a fresh idle masked another still-working session
        # -> a premature "finished" on the first end and none on the last.) A
        # hung/crashed working session is dropped by activity_fresh after
        # STATE_STALE_S, so it can't pin the aggregate to working forever.
        if activity_fresh:
            if act == "working":
                any_working = True
            else:
                any_idle = True

    for dead in [s for s in _waiting_seen if s not in live_waiting_sids]:
        del _waiting_seen[dead]

    activity = 1 if any_working else (0 if any_idle else 3)

    waiting.sort(key=lambda x: x[0])

    # Build the dashboard list (ss): resolve each session's state, then sort
    # attention-first (waiting/question before working before idle) so the
    # 512 B drop ladder trims the least-interesting rows last.
    gated_waiting = {w[1] for w in waiting}
    _RANK = {2: 0, 4: 1, 1: 2}  # cs -> sort rank (idle/other default to 3, last)
    sessions: list[tuple[int, float, str, int]] = []
    for sid, name, dialog, dfresh, act, afresh, ts in session_recs:
        if sid in gated_waiting:
            cs = 2
        elif dialog == "asking" and dfresh:
            cs = 4
        elif act == "working" and afresh:
            cs = 1
        else:
            cs = 0
        sessions.append((_RANK.get(cs, 3), ts, name, cs))
    sessions.sort(key=lambda x: (x[0], -x[1]))
    ss = [{"n": name, "cs": cs} for (_, _ts, name, cs) in sessions[:MAX_SESSIONS]]

    if waiting:
        q = [{"tn": tool, "td": detail, "sn": name}
             for (_, _sid, tool, detail, name) in waiting[:MAX_Q]]
        result = {
            "cs": 2,
            "aq": len(waiting),
            "q": q,
            "ss": ss,
            "_queue": [w[1] for w in waiting],
        }
    elif asking_kind is not None:
        tn = _ASK_KIND_TO_TOOL.get(asking_kind, "AskUserQuestion")
        result = {"cs": 4, "aq": 1, "q": [{"tn": tn, "td": "", "sn": asking_name}],
                  "ss": ss, "_queue": []}
    else:
        result = {"cs": activity, "aq": 0, "q": [], "ss": ss, "_queue": []}
    # _maturing rides on whatever state fires (a young waiting dialog still shows
    # as working/idle here) so the poll loop knows to re-check fast. Internal —
    # stripped before the BLE send, like _queue.
    result["_maturing"] = maturing
    return result


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
        # The device RX buffer is 512 B and silently truncates a longer write
        # (-> JSON parse fails). Drop ladder, cheapest loss first, so usage +
        # aggregate state always get through: (1) approval-queue session names
        # ("sn" — the dashboard "ss" already carries names), then (2) trim the
        # dashboard list ("ss") from the tail (it is sorted attention-first, so
        # the least-interesting idle rows go first).
        if len(data) > 500 and payload.get("q"):
            for entry in payload["q"]:
                entry.pop("sn", None)
            data = json.dumps(payload, separators=(",", ":")).encode()
        while len(data) > 500 and payload.get("ss"):
            payload["ss"].pop()
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

    last_usage: dict = {}     # last successful usage payload (ok=True)
    last_error: dict = {}      # current error overlay (ok=False, ec, em) or {} when healthy
    last_pushed: str | None = None
    used = {"ok": False}
    dialog_maturing = {"on": False}   # set by push(): a waiting dialog is still
                                      # firming up -> poll_loop re-checks fast.

    # Pre-baked auth overlay — used for both a missing token and a 401/403.
    auth_error = _error_payload("auth", "Not logged in\nRun: claude login")

    session_stop = asyncio.Event()

    async def forward_global_stop() -> None:
        await stop_event.wait()
        session_stop.set()

    async def push() -> None:
        nonlocal last_pushed
        if not last_usage and not last_error:
            return
        state = _claude_state_fields()
        dialog_maturing["on"] = bool(state.pop("_maturing", False))
        state.pop("_queue", None)
        # last_error wins over last_usage (its ok=False + em override the stale
        # numbers), so the device shows the error while keeping the bars beneath.
        merged = {**last_usage, **last_error, **state}
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
                    last_error.clear()
                    last_error.update(auth_error)
                    if tray_state is not None:
                        tray_state.set_error("token expired — run claude login")
                else:
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        last_error.clear()
                        last_error.update(auth_error)
                        if tray_state is not None:
                            tray_state.set_error("token expired — run claude login")
                    else:
                        # poll_api now always returns a dict: a usage payload
                        # (ok=True) or an error overlay (ok=False, ec, em).
                        if payload.get("ok"):
                            last_usage.clear()
                            last_usage.update(payload)
                            last_error.clear()
                        else:
                            last_error.clear()
                            last_error.update(payload)
                last_poll = time.time()
            # Re-evaluate live Claude state every tick, not just on poll. The
            # waiting-dialog stability gate needs repeated reads to mature, and
            # a lone permission prompt fires no further file-change events for
            # watch_loop to ride on — so without this a single prompt would sit
            # invisible until it went stale. Cheap: push() no-ops when the
            # serialized payload is unchanged.
            await push()
            # While a permission/waiting dialog is maturing, re-check every
            # FAST_TICK so it surfaces near the DIALOG_MIN_AGE_S floor instead of
            # waiting up to two slow TICKs. The slow tick doubles as the BLE
            # liveness probe; fast cycles skip the ping to spare the link.
            wait = FAST_TICK if dialog_maturing["on"] else TICK
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=wait)
            except asyncio.TimeoutError:
                if wait >= TICK and not await session.ping():
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
