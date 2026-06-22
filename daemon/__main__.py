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
    import argparse

    parser = argparse.ArgumentParser(prog="python -m daemon")
    parser.add_argument(
        "--ota", metavar="FIRMWARE_BIN",
        help="push this firmware .bin to the device over BLE, then exit. "
             "Stop any running daemon first (it holds the BLE link).",
    )
    parser.add_argument(
        "--board", metavar="ENV",
        help="expected board id (PlatformIO env name) for the OTA board guard.",
    )
    args = parser.parse_args()

    backend = _select_backend()

    if args.ota:
        from daemon import ota_push
        try:
            sys.exit(asyncio.run(ota_push.run(backend, args.ota, args.board)))
        except KeyboardInterrupt:
            sys.exit(130)

    from daemon import core
    try:
        asyncio.run(core.main(backend))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main_cli()
