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
