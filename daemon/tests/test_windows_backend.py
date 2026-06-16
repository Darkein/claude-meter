import os
from pathlib import Path
import pytest
from daemon.backends.windows import WindowsBackend, _windows_credential_candidates

def test_make_client_passes_winrt_kwargs(monkeypatch):
    captured = {}
    class FakeClient:
        def __init__(self, t, **kw): captured["t"], captured["kw"] = t, kw
    monkeypatch.setattr("daemon.backends.windows.BleakClient", FakeClient)
    WindowsBackend().make_client(object())
    assert captured["kw"] == {"address_type": "random", "use_cached_services": False}

def test_credential_override(monkeypatch, tmp_path):
    f = tmp_path / "c.json"; monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(f))
    assert _windows_credential_candidates() == [f]
