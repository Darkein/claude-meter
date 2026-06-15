#!/bin/bash
# Clawdmeter PreToolUse hook. Fires AFTER a tool-permission prompt is answered
# (granted) and BEFORE the tool executes — so it's the earliest moment we can
# clear the device's "waiting" (permission pending) screen. Without it, the
# screen lingers until PostToolUse, i.e. until the (possibly slow) tool finishes
# running; the user wants it gone the instant the decision is made.
#
# Records "working", EXCEPT for AskUserQuestion and ExitPlanMode: those tools'
# own PreToolUse matchers set "asking" (Claude is blocked on a user choice / plan
# approval), and both the specific matcher and this "*" matcher fire for the same
# event — so we must NOT overwrite "asking" with "working" here.
#
# Non-blocking, emits no permission decision. Always exits 0 (PreToolUse exit 0
# with no JSON = no opinion; the tool proceeds normally).
set -u
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 0
TOOL=$(printf '%s' "$IN" | jq -r '.tool_name // ""')
case "$TOOL" in AskUserQuestion|ExitPlanMode) exit 0 ;; esac   # let the asking hook own these

DIR="$HOME/.config/claude-usage-monitor/state"
mkdir -p "$DIR"
TMP="$DIR/$SID.json.tmp"
jq -nc --arg sid "$SID" --argjson ts "$(date +%s)" \
  '{state:"working", tool:"", ts:$ts, sid:$sid}' > "$TMP" \
  && mv -f "$TMP" "$DIR/$SID.json"
exit 0
