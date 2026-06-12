#!/bin/bash
# Clawdmeter SessionEnd hook. Removes the per-session state file so a finished
# session leaves no stale entry behind (otherwise it lingers until the daemon's
# 24h prune). Non-blocking.
set -u
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
rm -f "$HOME/.config/claude-usage-monitor/state/$SID.json"
exit 0
