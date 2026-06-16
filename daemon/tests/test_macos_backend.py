import sys, pytest

@pytest.mark.skipif(sys.platform != "darwin", reason="CoreBluetooth only on macOS")
def test_macos_backend_importable_and_make_client_plain():
    from daemon.backends.macos import MacOSBackend
    b = MacOSBackend()
    assert b.name == "macos"

def test_macos_module_imports_without_corebluetooth():
    # Module-level import must not require CoreBluetooth (it's imported lazily
    # inside retrieve_connected). Importing the module on Linux must succeed.
    import importlib
    mod = importlib.import_module("daemon.backends.macos")
    assert hasattr(mod, "MacOSBackend")
