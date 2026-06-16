#!/usr/bin/env python3
"""Unit tests for core connect_and_run reconnect hardening.

Covers:
  ping-keepalive dead-link detection (replaces Windows D-01 retry wrapper)
  tray_state integration
  _wait_first / _next_backoff helpers
  OSError tolerance on start_notify / write_payload

Run: python -m pytest daemon/tests/test_windows_reconnect.py -x -q
"""
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from bleak.exc import BleakError

from daemon.core import (
    AuthError,
    Session,
    _wait_first,
    connect_and_run,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run(coro):
    return asyncio.run(coro)


def _make_device(address="AA:BB:CC:DD:EE:FF"):
    device = MagicMock()
    device.address = address
    return device


def _make_backend(client, token="fake-token"):
    backend = MagicMock()
    backend.make_client.return_value = client
    backend.read_token.return_value = token
    return backend


async def _make_event(set_):
    ev = asyncio.Event()
    if set_:
        ev.set()
    return ev


# ---------------------------------------------------------------------------
# Ping-keepalive: dead link detected via ping failure
# ---------------------------------------------------------------------------

def test_ping_failure_ends_session():
    """When ping() returns False (link dead), connect_and_run terminates promptly.

    The session should end (not hang indefinitely) when the keepalive ping fails.
    We verify it completes within a short timeout, and disconnect is called.
    """
    from daemon import core as core_mod
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock()

    stop_event = asyncio.run(_make_event(False))
    # Don't provide a token so no writes happen — result stays False
    backend = _make_backend(client, token=None)

    async def fake_ping(self_):
        return False  # dead link

    # Patch asyncio.wait_for to trigger TimeoutError immediately (so ping runs)
    async def fast_wait_for(coro, timeout):
        raise asyncio.TimeoutError()

    with patch("daemon.core.awatch", return_value=_empty_aiter()), \
         patch("daemon.core.asyncio.wait_for", side_effect=fast_wait_for), \
         patch.object(Session, "ping", new=fake_ping):
        result = _run(connect_and_run(backend, _make_device(), stop_event))

    # Session ended (disconnect called), no writes occurred → False
    assert result is False
    assert client.disconnect.call_count >= 1


def test_connect_failure_returns_false():
    """BleakError on connect returns False immediately."""
    client = AsyncMock()
    client.connect = AsyncMock(side_effect=BleakError("Unreachable"))
    client.disconnect = AsyncMock()

    stop_event = asyncio.run(_make_event(False))
    backend = _make_backend(client)

    result = _run(connect_and_run(backend, _make_device(), stop_event))
    assert result is False


def test_connect_success_on_stop_returns_false():
    """Connecting successfully but stopping before any write returns False."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()

    stop_event = asyncio.run(_make_event(True))  # already set
    backend = _make_backend(client)

    with patch("daemon.core.poll_api", new=AsyncMock(return_value={"ok": True, "s": 1})), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        result = _run(connect_and_run(backend, _make_device(), stop_event))

    assert result is False


def test_successful_write_returns_true():
    """A successful write to the device returns True."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock()

    stop_event = asyncio.run(_make_event(False))
    backend = _make_backend(client)

    writes = [0]
    async def fake_write(char, data, **kw):
        writes[0] += 1
        stop_event.set()  # stop after first write

    client.write_gatt_char.side_effect = fake_write

    with patch("daemon.core.poll_api", new=AsyncMock(return_value={"ok": True, "s": 1})), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        result = _run(connect_and_run(backend, _make_device(), stop_event))

    assert result is True


def test_backend_make_client_called_with_target():
    """connect_and_run delegates BleakClient creation to backend.make_client."""
    device = _make_device("AA:BB:CC:DD:EE:FF")
    client = AsyncMock()
    client.connect = AsyncMock()
    client.disconnect = AsyncMock()
    client.is_connected = True
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock()

    stop_event = asyncio.run(_make_event(True))
    backend = _make_backend(client)

    with patch("daemon.core.poll_api", new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        _run(connect_and_run(backend, device, stop_event))

    backend.make_client.assert_called_once_with(device)


def test_disconnect_called_in_finally():
    """client.disconnect() is always called in the finally block."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()

    stop_event = asyncio.run(_make_event(True))
    backend = _make_backend(client)

    with patch("daemon.core.poll_api", new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        _run(connect_and_run(backend, _make_device(), stop_event))

    assert client.disconnect.call_count >= 1


# ---------------------------------------------------------------------------
# start_notify OSError must not crash (G-03-01)
# ---------------------------------------------------------------------------

def test_start_notify_oserror_does_not_crash_connect_and_run():
    """G-03-01 regression: on post-power-cycle reconnect, start_notify() CCCD
    write can raise a raw OSError. Must degrade gracefully."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock(
        side_effect=OSError(-2147023673, "The operation was canceled by the user.")
    )

    stop_event = asyncio.run(_make_event(True))
    backend = _make_backend(client)

    with patch("daemon.core.poll_api", new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        result = _run(connect_and_run(backend, _make_device(), stop_event))

    assert client.start_notify.call_count == 1
    assert client.disconnect.call_count >= 1


# ---------------------------------------------------------------------------
# write_payload OSError must not crash (SC#2)
# ---------------------------------------------------------------------------

def test_write_payload_oserror_returns_false_not_raises():
    """SC#2 regression: write_gatt_char can raise a raw OSError."""
    client = AsyncMock()
    client.write_gatt_char = AsyncMock(
        side_effect=OSError(-2147023673, "The operation was canceled by the user.")
    )
    session = Session(client)
    assert _run(session.write_payload({"ok": True})) is False


def test_write_payload_bleak_error_still_returns_false():
    """Pre-existing BleakError path must keep returning False."""
    client = AsyncMock()
    client.write_gatt_char = AsyncMock(side_effect=BleakError("disconnected"))
    session = Session(client)
    assert _run(session.write_payload({"ok": True})) is False


# ---------------------------------------------------------------------------
# _wait_first (SC#3 graceful Quit)
# ---------------------------------------------------------------------------

def test_wait_first_returns_immediately_when_an_event_is_set():
    async def go():
        refresh = asyncio.Event()
        stop = asyncio.Event()
        stop.set()
        await asyncio.wait_for(_wait_first(refresh, stop, timeout=30.0), timeout=2.0)
        assert not refresh.is_set()
    _run(go())


def test_wait_first_returns_after_timeout_when_no_event_set():
    async def go():
        await asyncio.wait_for(
            _wait_first(asyncio.Event(), asyncio.Event(), timeout=0.05), timeout=2.0
        )
    _run(go())


# ---------------------------------------------------------------------------
# SC#5: transient poll failure must NOT toast "token expired"
# ---------------------------------------------------------------------------

def test_transient_poll_failure_does_not_set_error():
    """poll_api returning None (network/DNS, timeout, 5xx, 429) is transient."""
    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    tray_state = MagicMock()
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    backend = _make_backend(client)

    async def fake_poll(_token):
        stop_event.set()
        return None

    with patch("daemon.core.poll_api", new=fake_poll), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        _run(connect_and_run(backend, device, stop_event, tray_state))

    tray_state.set_error.assert_not_called()
    tray_state.set_connected.assert_not_called()


def test_auth_error_sets_token_expired():
    """A genuine 401/403 surfaces as AuthError and flips the tray to error state."""
    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    tray_state = MagicMock()
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    backend = _make_backend(client)

    async def fake_poll(_token):
        stop_event.set()
        raise AuthError(401)

    with patch("daemon.core.poll_api", new=fake_poll), \
         patch("daemon.core.awatch", return_value=_empty_aiter()):
        _run(connect_and_run(backend, device, stop_event, tray_state))

    tray_state.set_error.assert_called_once_with("token expired — run claude login")


# ---------------------------------------------------------------------------
# _next_backoff
# ---------------------------------------------------------------------------

def test_next_backoff_slow_search_doubles_to_60():
    from daemon.core import _next_backoff
    values = []
    b = 1
    for _ in range(10):
        b = _next_backoff(b, 60)
        values.append(b)
    assert values == [2, 4, 8, 16, 32, 60, 60, 60, 60, 60]
    assert max(values) <= 60


def test_next_backoff_one_to_two():
    from daemon.core import _next_backoff
    assert _next_backoff(1, 60) == 2


def test_next_backoff_at_cap_stays():
    from daemon.core import _next_backoff
    assert _next_backoff(8, 8) == 8


def test_main_scan_miss_uses_backoff():
    """When discover_target returns None, asyncio.wait_for receives increasing backoff."""
    from daemon import core

    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    recorded_timeouts = []
    call_count = [0]
    MAX_CALLS = 3

    async def fake_discover(skip_addr=None):
        return None

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= MAX_CALLS and internal_stop_event[0] is not None:
            internal_stop_event[0].set()
        raise asyncio.TimeoutError()

    backend = MagicMock()
    backend.single_instance.return_value = object()
    backend.discover_target = AsyncMock(side_effect=fake_discover)

    with patch("daemon.core.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.core.asyncio.wait_for", side_effect=fake_wait_for):
        _run(core.main(backend))

    assert len(recorded_timeouts) >= 2
    assert recorded_timeouts[0] == 1
    assert recorded_timeouts[1] == 2
    assert all(t <= 60 for t in recorded_timeouts)


def test_main_connect_fail_uses_backoff():
    """When connect_and_run returns False, asyncio.wait_for receives backoff timeouts."""
    from daemon import core

    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    fake_device = _make_device()
    recorded_timeouts = []
    call_count = [0]
    MAX_CALLS = 3

    async def fake_connect_and_run(backend, device, event, tray_state=None):
        return False

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= MAX_CALLS and internal_stop_event[0] is not None:
            internal_stop_event[0].set()
        raise asyncio.TimeoutError()

    backend = MagicMock()
    backend.single_instance.return_value = object()
    backend.discover_target = AsyncMock(return_value=fake_device)
    backend.on_target_failed.return_value = None

    with patch("daemon.core.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.core.connect_and_run", side_effect=fake_connect_and_run), \
         patch("daemon.core.asyncio.wait_for", side_effect=fake_wait_for):
        _run(core.main(backend))

    assert len(recorded_timeouts) >= 2
    assert recorded_timeouts[0] == 1
    assert recorded_timeouts[1] == 2
    assert all(t <= 60 for t in recorded_timeouts)


def test_main_backoff_reset_on_success():
    """A successful connect_and_run (returns True) resets backoff to 1."""
    from daemon import core

    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    fake_device = _make_device()
    recorded_timeouts = []
    call_count = [0]

    connect_results = [False, True, False]
    connect_idx = [0]

    async def fake_connect_and_run(backend, device, event, tray_state=None):
        idx = connect_idx[0]; connect_idx[0] += 1
        return connect_results[idx] if idx < len(connect_results) else False

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= 2 and internal_stop_event[0] is not None:
            internal_stop_event[0].set()
        raise asyncio.TimeoutError()

    backend = MagicMock()
    backend.single_instance.return_value = object()
    backend.discover_target = AsyncMock(return_value=fake_device)
    backend.on_target_failed.return_value = None

    with patch("daemon.core.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.core.connect_and_run", side_effect=fake_connect_and_run), \
         patch("daemon.core.asyncio.wait_for", side_effect=fake_wait_for):
        _run(core.main(backend))

    assert len(recorded_timeouts) >= 2
    assert recorded_timeouts[0] == 1, f"Expected 1 on first fail, got {recorded_timeouts[0]}"
    assert recorded_timeouts[1] == 1, f"Expected 1 after success reset, got {recorded_timeouts[1]}"


def test_main_no_saved_addr_file_or_skip_addr():
    """core.main() does not reference SAVED_ADDR_FILE (Linux-only cached address)."""
    import inspect
    from daemon import core as mod

    source = inspect.getsource(mod.main)
    assert "SAVED_ADDR_FILE" not in source, "main() must not reference SAVED_ADDR_FILE"


def test_requirements_windows_contains_required_deps():
    req_path = Path(__file__).parent.parent / "requirements-windows.txt"
    content = req_path.read_text()
    lines = {line.strip().lower() for line in content.splitlines()
             if line.strip() and not line.strip().startswith("#")}
    assert "bleak" in lines
    assert "httpx" in lines
    assert "pystray" in lines
    assert "pillow" in lines
    assert "winreg" not in lines


# ---------------------------------------------------------------------------
# Helper async generator
# ---------------------------------------------------------------------------

async def _empty_aiter():
    if False:
        yield
