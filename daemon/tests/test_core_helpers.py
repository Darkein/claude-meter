import json, time
import pytest
from daemon import core

def test_poll_api_exported_and_autherror():
    assert hasattr(core, "AuthError")
    assert issubclass(core.AuthError, Exception)

def test_extract_token_nested():
    blob = json.dumps({"claudeAiOauth": {"accessToken": "sk-ant-xxxxxxxxxxxxxxxxxxxx"}})
    assert core._extract_access_token(blob) == "sk-ant-xxxxxxxxxxxxxxxxxxxx"

def test_state_fields_empty_dir_returns_none_status(tmp_path, monkeypatch):
    monkeypatch.setattr(core, "STATE_DIR", tmp_path)
    out = core._claude_state_fields()
    assert out["cs"] == 3 and out["aq"] == 0 and out["q"] == []

def test_device_name_unified():
    assert core.DEVICE_NAME == "Clawdmeter"

def test_core_has_session_waitfirst_nextbackoff():
    assert hasattr(core, "Session")
    assert hasattr(core, "_wait_first")
    assert core._next_backoff(1, 60) == 2 and core._next_backoff(40, 60) == 60

import asyncio
def test_wait_first_returns_on_set_event():
    async def go():
        e1, e2 = asyncio.Event(), asyncio.Event()
        e1.set()
        await asyncio.wait_for(core._wait_first(e1, e2, timeout=5), timeout=1)
    asyncio.run(go())


# ---------------------------------------------------------------------------
# _claude_state_fields — two-field (activity + dialog) lifecycle coverage
# ---------------------------------------------------------------------------

@pytest.fixture
def state_dir(tmp_path, monkeypatch):
    """Isolated STATE_DIR + a clean _waiting_seen for each test."""
    monkeypatch.setattr(core, "STATE_DIR", tmp_path)
    core._waiting_seen.clear()
    return tmp_path


def _write(d, sid, **fields):
    (d / f"{sid}.json").write_text(json.dumps({"sid": sid, **fields}))


NOW = None  # set per-test via time.time()


def test_asking_question_reported_immediately(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="question", dialog_ts=now)
    out = core._claude_state_fields()
    assert out["cs"] == 4 and out["aq"] == 1
    assert out["q"] == [{"tn": "AskUserQuestion", "td": ""}]


def test_asking_plan_maps_to_exitplanmode(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="plan", dialog_ts=now)
    out = core._claude_state_fields()
    assert out["cs"] == 4 and out["q"][0]["tn"] == "ExitPlanMode"


def test_REGRESSION_turnpause_idle_does_not_hide_asking(state_dir):
    # The reported bug: a turn-pause Stop wrote activity=idle into the SAME file
    # while the question dialog was still open. dialog must still win -> cs=4.
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="question", dialog_ts=now,
           activity="idle", activity_ts=now)
    out = core._claude_state_fields()
    assert out["cs"] == 4


def test_asking_stale_falls_back_to_fresh_activity(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="question", dialog_ts=now - 100,
           activity="idle", activity_ts=now)
    out = core._claude_state_fields()
    assert out["cs"] == 0  # stale dialog ignored, fresh idle activity wins


def test_fully_stale_file_is_pruned(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="question", dialog_ts=now - 100,
           activity="idle", activity_ts=now - 9999)
    out = core._claude_state_fields()
    assert out["cs"] == 3
    assert not (state_dir / "a.json").exists()  # dead file removed


def test_waiting_needs_two_stable_reads(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="waiting", kind="permission", tool="Bash",
           detail="rm -rf /tmp/x", dialog_ts=now - 3,   # older than DIALOG_MIN_AGE_S
           activity="working", activity_ts=now - 3)
    # First read: not yet stable -> falls back to the file's activity (working).
    out1 = core._claude_state_fields()
    assert out1["cs"] == 1
    # Second read with the same dialog_ts: matured -> permission pending.
    out2 = core._claude_state_fields()
    assert out2["cs"] == 2 and out2["aq"] == 1
    assert out2["q"] == [{"tn": "Bash", "td": "rm -rf /tmp/x"}]


def test_waiting_too_young_not_counted(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="waiting", kind="permission", tool="Bash",
           dialog_ts=now, activity="working", activity_ts=now)  # age < 2.5s
    assert core._claude_state_fields()["cs"] == 1  # not 2 yet


def test_waiting_moving_ts_never_matures(state_dir):
    # An auto-grant stream advances dialog_ts every PermissionRequest; the
    # stability gate resets, so it never reaches cs=2 (treated as working).
    now = time.time()
    _write(state_dir, "a", dialog="waiting", kind="permission", tool="Bash",
           dialog_ts=now - 4, activity="working", activity_ts=now - 4)
    assert core._claude_state_fields()["cs"] == 1
    _write(state_dir, "a", dialog="waiting", kind="permission", tool="Bash",
           dialog_ts=now - 3, activity="working", activity_ts=now - 3)  # ts moved
    assert core._claude_state_fields()["cs"] == 1  # reset, still not matured


def test_waiting_priority_over_asking(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="waiting", kind="permission", tool="Bash",
           dialog_ts=now - 3, activity="working", activity_ts=now - 3)
    _write(state_dir, "b", dialog="asking", kind="question", dialog_ts=now)
    core._claude_state_fields()              # mature the waiting (2 reads)
    assert core._claude_state_fields()["cs"] == 2


def test_asking_priority_over_activity_concurrent_sessions(state_dir):
    now = time.time()
    _write(state_dir, "a", dialog="asking", kind="question", dialog_ts=now)
    _write(state_dir, "b", dialog="none", activity="working", activity_ts=now)
    assert core._claude_state_fields()["cs"] == 4


def test_working_dominates_idle_across_sessions(state_dir):
    # Two concurrent sessions. The aggregate stays "working" until they have ALL
    # gone idle: a session finishing (fresh idle) must not mask another that is
    # still working, or the device fires "finished" on the first end and misses
    # the last. The done chime should fire exactly once, on the last end.
    now = time.time()
    _write(state_dir, "a", dialog="none", activity="working", activity_ts=now - 10)
    _write(state_dir, "b", dialog="none", activity="idle", activity_ts=now)
    assert core._claude_state_fields()["cs"] == 1  # a still working -> working
    _write(state_dir, "a", dialog="none", activity="idle", activity_ts=now)
    assert core._claude_state_fields()["cs"] == 0  # both idle now -> finished
