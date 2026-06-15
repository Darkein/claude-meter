#!/bin/bash
# Clawdmeter session-state hook (parameterised). $1 = the state to record:
#   working  — Claude is actively processing (prompt submitted, tool ran, MCP
#              elicitation answered)
#   idle     — turn finished / waiting for user input
#   asking   — Claude is blocked on an AskUserQuestion or MCP elicitation_dialog,
#              waiting for the user to choose an answer. Cleared by the next
#              PostToolUse (working) once the question is answered.
#
# Reads the hook JSON on stdin, extracts session_id, and atomically writes the
# per-session state file the claude-usage daemon watches. Non-blocking, emits no
# permission decision. The "waiting" (permission prompt) state is owned by
# state-perm.sh instead (it also needs the pending tool name).
set -u
STATE="${1:-working}"
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
DIR="$HOME/.config/claude-usage-monitor/state"
mkdir -p "$DIR"
FILE="$DIR/$SID.json"

# Explicit dialog clear: an "idle" signal (Stop / StopFailure /
# Notification:idle_prompt, or a declined/cancelled ElicitationResult routed
# here by state-elicit.sh) arriving while the session's last state was a
# pending dialog means the prompt was just resolved or DISMISSED (incl. ESC).
# Drop the file so the device's approval/question screen clears the instant the
# signal fires, instead of lingering until the daemon's DIALOG_STALE_S prune.
if [ "$STATE" = "idle" ] && [ -f "$FILE" ]; then
  prev=$(jq -r '.state // empty' "$FILE" 2>/dev/null)
  if [ "$prev" = "waiting" ] || [ "$prev" = "asking" ]; then
    rm -f "$FILE"
    exit 0
  fi
fi

TMP="$FILE.tmp"
jq -nc --arg s "$STATE" --arg sid "$SID" --argjson ts "$(date +%s)" \
  '{state:$s, tool:"", ts:$ts, sid:$sid}' > "$TMP" \
  && mv -f "$TMP" "$FILE"
exit 0
