"""`python -m daemon` entry. Selects the platform backend and runs core.main."""
import asyncio
import sys


def _select_backend():
    if sys.platform == "darwin":
        from daemon.backends.macos import MacOSBackend
        return MacOSBackend()
    if sys.platform == "win32":
        from daemon.backends.windows import WindowsBackend
        return WindowsBackend()
    if sys.platform != "linux":
        # bleak's WinRT path is the only Windows-native one; warn on odd platforms.
        print(
            "Warning: running under Linux/WSL — WinRT BLE will not be available.",
            file=sys.stderr,
            flush=True,
        )
    from daemon.backends.linux import LinuxBackend
    return LinuxBackend()


def main_cli() -> None:
    backend = _select_backend()
    from daemon import core
    try:
        asyncio.run(core.main(backend))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main_cli()
