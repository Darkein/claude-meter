#!/bin/bash
# Build a firmware env and push it to the device over BLE OTA.
# Usage: ./tools/build_and_ota.sh [env] [python]
#   env    PlatformIO env to build (default: waveshare_amoled_216_c6)
#   python interpreter running the daemon (default: daemon/.venv/bin/python)
#
# Stop any running daemon FIRST — it holds the BLE link and the single-instance
# lock. See docs/porting/ota.md.
set -euo pipefail

ENV="${1:-waveshare_amoled_216_c6}"
PY="${2:-daemon/.venv/bin/python}"
[ -x "$PY" ] || PY=python3

PIO="$(command -v pio || true)"
[ -n "$PIO" ] || PIO="$HOME/.platformio/penv/bin/pio"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="firmware/.pio/build/$ENV/firmware.bin"

echo "==> Building $ENV"
"$PIO" run -d firmware -e "$ENV"

echo "==> Pushing $BIN over BLE (stop the daemon first if it's running)"
"$PY" -m daemon --ota "$BIN" --board "$ENV"
