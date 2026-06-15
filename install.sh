#!/bin/bash
# Linux installer for the Clawdmeter daemon (Python + bleak + systemd user unit).
# Mirrors install-mac.sh but uses a systemd --user service instead of launchd.
# The Python daemon (daemon/claude_usage_daemon.py) is the only implementation:
# it polls usage AND drives the live Claude Code state pipeline via the hook
# state files. (The old bash daemon was usage-only and has been removed.)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="claude-usage-daemon"
SERVICE_FILE="$SCRIPT_DIR/daemon/$SERVICE_NAME.service"
USER_SERVICE_DIR="$HOME/.config/systemd/user"
VENV_DIR="$SCRIPT_DIR/daemon/.venv"
DAEMON_PY="$SCRIPT_DIR/daemon/claude_usage_daemon.py"

echo "=== Claude Usage Tracker - Install (Linux) ==="
echo ""

echo "[1/4] Checking dependencies..."
for cmd in python3 curl; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required but not installed"; exit 1; }
done
echo "  OK"
echo ""

echo "[2/4] Creating Python virtualenv at daemon/.venv ..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet -r "$SCRIPT_DIR/daemon/requirements.txt"
PYTHON_BIN="$VENV_DIR/bin/python"
echo "  OK ($PYTHON_BIN)"
echo ""

echo "[2b/4] Installing Claude Code hooks (live-state pipeline)..."
"$SCRIPT_DIR/daemon/install-hooks.sh"
echo ""

echo "[3/4] Installing systemd user service..."
mkdir -p "$USER_SERVICE_DIR"
# Render ExecStart = <venv python> <daemon.py>. Use '#' as the sed delimiter so
# absolute paths containing '/' don't need escaping.
sed "s#DAEMON_PATH#${PYTHON_BIN} ${DAEMON_PY}#g" "$SERVICE_FILE" \
    > "$USER_SERVICE_DIR/$SERVICE_NAME.service"
systemctl --user daemon-reload

echo "[4/4] Enabling service..."
systemctl --user enable "$SERVICE_NAME"

echo ""
echo "=== Done! ==="
echo ""
echo "The daemon starts automatically at login and connects to 'Clawdmeter'"
echo "over Bluetooth Low Energy."
echo ""
echo "First-time Bluetooth pairing:"
echo "  1. Power on the device."
echo "  2. Run: bluetoothctl scan le"
echo "  3. Find 'Clawdmeter' and note the MAC address."
echo "  4. Run: bluetoothctl pair <MAC> && bluetoothctl trust <MAC>"
echo "  5. Start the daemon: systemctl --user start $SERVICE_NAME"
echo ""
echo "Useful commands:"
echo "  systemctl --user status $SERVICE_NAME    # check status"
echo "  journalctl --user -u $SERVICE_NAME -f    # view logs"
echo "  systemctl --user restart $SERVICE_NAME   # restart"
echo "  systemctl --user stop $SERVICE_NAME      # stop"
echo ""
