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
