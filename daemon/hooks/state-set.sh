#!/bin/bash
# Claude Meter session-state hook (parameterised). $1 = the signal to record:
#   working  — Claude is making progress (prompt submitted, tool started/ended,
#              MCP elicitation accepted). Sets activity=working AND clears any
#              pending dialog (a tool running / answer submitted resolves it).
#   idle     — the turn paused or finished (Stop / StopFailure /
#              Notification:idle_prompt). Sets activity=idle ONLY.
#   asking   — Claude is blocked on a user choice. $2 = kind (question | plan |
#              elicitation). Sets dialog=asking; activity is left untouched.
#
# Two independent facts live in the per-session file so they can never clobber
# each other:
#   activity : working | idle      (turn progress)
#   dialog   : none | waiting | asking  (pending UI; takes priority in the daemon)
#
# WHY idle must NOT clear an "asking" dialog (the bug this fixes):
#   When Claude opens an AskUserQuestion / ExitPlanMode dialog the turn PAUSES
#   for input and Claude Code emits a Stop / idle_prompt *while the dialog is
#   still open*. The old single-field design saw that idle, assumed the dialog
#   was resolved, and deleted it -> the device showed "idle" for the whole
#   question. So a bare idle now sets activity only and never touches "asking".
#   "asking" clears on a positive resolution (the answer fires PostToolUse ->
#   working), an explicit dismissal (state-elicit decline/cancel), SessionEnd,
#   or the daemon's short dialog TTL.
#
# WHY idle DOES clear a "waiting" (permission) dialog:
#   A permission prompt being open emits NO Stop (verified: a 76s permission
#   wait fired no Stop). So an idle while dialog=waiting can only mean the
#   prompt was just resolved with no tool run -> a DENY or ESC. There is no
#   other hook for that case, so idle is its legitimate clear signal.
#
# Reads the hook JSON on stdin, extracts session_id, merges into the per-session
# file the daemon watches (read-modify-write, atomic). Non-blocking, emits no
# permission decision. The "waiting" state is owned by state-perm.sh (it also
# needs the pending tool name + detail).
set -u
STATE="${1:-working}"
KIND="${2:-}"          # asking only: question | plan | elicitation
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
CWD=$(printf '%s' "$IN" | jq -r '.cwd // ""')   # project folder -> device label
NAME=$(basename "$CWD" 2>/dev/null); [ -z "$CWD" ] && NAME=""; NAME=${NAME:0:20}
DIR="$HOME/.config/claude-meter/state"
mkdir -p "$DIR"
FILE="$DIR/$SID.json"
TMP="$FILE.tmp"
NOW=$(date +%s)
EXIST=$(cat "$FILE" 2>/dev/null); [ -z "$EXIST" ] && EXIST='{}'

case "$STATE" in
  working)
    printf '%s' "$EXIST" | jq -c --argjson ts "$NOW" --arg sid "$SID" --arg name "$NAME" \
      '. + {activity:"working", activity_ts:$ts,
            dialog:"none", kind:"", tool:"", detail:"", sid:$sid, name:$name}' \
      > "$TMP" && mv -f "$TMP" "$FILE"
    ;;
  idle)
    if [ "$(printf '%s' "$EXIST" | jq -r '.dialog // "none"')" = "waiting" ]; then
      # permission resolved with no tool run (deny / ESC) -> clear it.
      printf '%s' "$EXIST" | jq -c --argjson ts "$NOW" --arg sid "$SID" --arg name "$NAME" \
        '. + {activity:"idle", activity_ts:$ts,
              dialog:"none", kind:"", tool:"", detail:"", sid:$sid, name:$name}' \
        > "$TMP" && mv -f "$TMP" "$FILE"
    else
      # asking or none: set activity only, never touch the dialog.
      printf '%s' "$EXIST" | jq -c --argjson ts "$NOW" --arg sid "$SID" --arg name "$NAME" \
        '. + {activity:"idle", activity_ts:$ts, sid:$sid, name:$name}' \
        > "$TMP" && mv -f "$TMP" "$FILE"
    fi
    ;;
  asking)
    printf '%s' "$EXIST" | jq -c --argjson ts "$NOW" --arg sid "$SID" --arg k "$KIND" --arg name "$NAME" \
      '. + {dialog:"asking", kind:$k, tool:"", detail:"", dialog_ts:$ts, sid:$sid, name:$name}' \
      > "$TMP" && mv -f "$TMP" "$FILE"
    ;;
esac
exit 0
