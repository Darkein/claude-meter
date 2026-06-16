import subprocess, sys

def test_module_emits_non_windows_warning():
    # On Linux/macOS (non-win32 non-darwin or just non-win32): must print the warning
    # before entering the (hanging) scan loop.
    # On macOS (darwin): the linux branch is NOT taken, so this test is only meaningful
    # on Linux/WSL. On macOS, skip if platform is darwin.
    if sys.platform == "darwin":
        # On macOS, the warning is only emitted for non-darwin non-linux platforms.
        # This test verifies the __main__ module is runnable; warning behavior is linux-only.
        import daemon.__main__  # just import, don't run
        return
    try:
        r = subprocess.run([sys.executable, "-m", "daemon"],
                           capture_output=True, text=True, timeout=3, cwd=".")
        assert "WinRT BLE will not be available" in r.stderr
    except subprocess.TimeoutExpired as e:
        err = (e.stderr or b"").decode() if isinstance(e.stderr, bytes) else (e.stderr or "")
        assert "WinRT BLE will not be available" in err
