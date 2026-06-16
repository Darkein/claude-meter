#!/bin/bash
# Clawdmeter ElicitationResult hook. Fires after the user answers an MCP
# elicitation dialog, before the response goes back to the server. The stdin
# JSON carries .user_response.action (accept | decline | cancel).
#
#   accept           -> the user submitted values; the loop continues.
#                       activity=working, clear the dialog.
#   decline / cancel -> EXPLICIT dismissal. Clear the dialog immediately
#                       (dialog=none) and go idle. We do this directly rather
#                       than routing through state-set.sh's idle path, because
#                       that path deliberately does NOT clear an "asking" dialog
#                       (a bare idle can be a turn-pause Stop, not a dismissal).
#                       A decline/cancel IS a real dismissal, so it clears.
#
# Read-modify-write merge, atomic. Non-blocking, emits no decision. Exits 0.
set -u
IN=$(cat)
ACTION=$(printf '%s' "$IN" | jq -r '.user_response.action // "accept"')
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
DIR="$HOME/.config/claude-usage-monitor/state"
mkdir -p "$DIR"
FILE="$DIR/$SID.json"
TMP="$FILE.tmp"
NOW=$(date +%s)
EXIST=$(cat "$FILE" 2>/dev/null); [ -z "$EXIST" ] && EXIST='{}'

case "$ACTION" in
  decline|cancel) ACT="idle" ;;
  *)              ACT="working" ;;
esac
printf '%s' "$EXIST" | jq -c --argjson ts "$NOW" --arg sid "$SID" --arg act "$ACT" \
  '. + {activity:$act, activity_ts:$ts,
        dialog:"none", kind:"", tool:"", detail:"", sid:$sid}' \
  > "$TMP" && mv -f "$TMP" "$FILE"
exit 0
