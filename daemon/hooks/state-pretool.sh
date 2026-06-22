#!/bin/bash
# Claude Meter PreToolUse hook (matcher "*"). Fires before a tool runs — and,
# per Claude Code's lifecycle, BEFORE any permission dialog (PreToolUse runs
# ahead of the permission-mode check). So this is the "a tool is starting"
# signal: it marks the session working and clears any stale pending dialog.
# For a permission-gated tool the subsequent PermissionRequest re-sets
# dialog=waiting, so the approval screen still shows.
#
# Records working, EXCEPT for AskUserQuestion and ExitPlanMode: those tools'
# own PreToolUse matchers set dialog=asking, and both the specific matcher and
# this "*" matcher fire for the same event — so we must NOT clear "asking" here.
#
# Non-blocking, emits no permission decision. Always exits 0 (PreToolUse exit 0
# with no JSON = no opinion; the tool proceeds normally).
set -u
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
TOOL=$(printf '%s' "$IN" | jq -r '.tool_name // ""')
case "$TOOL" in AskUserQuestion|ExitPlanMode) exit 0 ;; esac   # let the asking hook own these
CWD=$(printf '%s' "$IN" | jq -r '.cwd // ""')   # project folder -> device label
NAME=$(basename "$CWD" 2>/dev/null); [ -z "$CWD" ] && NAME=""; NAME=${NAME:0:20}

DIR="$HOME/.config/claude-meter/state"
mkdir -p "$DIR"
FILE="$DIR/$SID.json"
TMP="$FILE.tmp"
EXIST=$(cat "$FILE" 2>/dev/null); [ -z "$EXIST" ] && EXIST='{}'
printf '%s' "$EXIST" | jq -c --argjson ts "$(date +%s)" --arg sid "$SID" --arg name "$NAME" \
  '. + {activity:"working", activity_ts:$ts,
        dialog:"none", kind:"", tool:"", detail:"", sid:$sid, name:$name}' \
  > "$TMP" && mv -f "$TMP" "$FILE"
exit 0
