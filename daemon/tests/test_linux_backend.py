import re
import pytest
from daemon.backends.linux import LinuxBackend
from daemon import core

def test_make_client_plain_no_extra_kwargs(monkeypatch):
    captured = {}
    class FakeClient:
        def __init__(self, target, **kw): captured["target"], captured["kw"] = target, kw
    monkeypatch.setattr("daemon.backends.linux.BleakClient", FakeClient)
    LinuxBackend().make_client("AA:BB:CC:DD:EE:FF")
    assert captured["kw"] == {}  # linux uses no special BleakClient kwargs

def test_load_cached_address_rejects_garbage(tmp_path, monkeypatch):
    b = LinuxBackend()
    f = tmp_path / "ble-address"
    f.write_text("not-a-mac")
    monkeypatch.setattr(b, "_addr_file", f)
    assert b._load_cached_address() is None
    assert not f.exists()
