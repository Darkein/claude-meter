#!/bin/bash
# Clawdmeter ElicitationResult hook. Fires after the user answers an MCP
# elicitation dialog, before the response goes back to the server. The stdin
# JSON carries .user_response.action (accept | decline | cancel).
#
#   accept  -> the user submitted values; the loop continues -> "working".
#   decline / cancel -> the user dismissed the dialog; the session is winding
#             down toward idle. Treat it like the ESC path: route to "idle" so
#             state-set.sh's delete-on-clear logic drops the session file and the
#             device's "asking" screen clears immediately, instead of flashing a
#             wrong "working" until the next Stop/idle_prompt arrives.
#
# We re-feed the original stdin to state-set.sh because it re-reads session_id
# from stdin (and the "idle" path needs the prior-state file to decide to delete).
# Non-blocking, emits no decision. Always exits 0.
set -u
IN=$(cat)
ACTION=$(printf '%s' "$IN" | jq -r '.user_response.action // "accept"')

DIR="$(cd "$(dirname "$0")" && pwd)"
case "$ACTION" in
  decline|cancel) STATE="idle" ;;
  *)              STATE="working" ;;
esac
printf '%s' "$IN" | "$DIR/state-set.sh" "$STATE"
exit 0
