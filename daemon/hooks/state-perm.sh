#!/bin/bash
# Clawdmeter PermissionRequest hook. Fires whenever Claude Code shows a tool
# permission dialog (Allow / Allow-always / No) — for EVERY prompt, focused or
# not, unlike Notification:permission_prompt which only fires when the OS emits
# a notification (window unfocused). Marks the session WAITING with the tool
# name so the daemon raises the device's approval screen.
#
# OBSERVE-ONLY. This hook must NEVER decide the permission:
#   - exit 0 with empty stdout would AUTO-ALLOW the tool (PermissionRequest
#     treats exit 0 as behavior:"allow"). We must NOT do that.
#   - exit 2 would block.
#   - any OTHER non-zero code = non-blocking, the native dialog shows normally
#     and the human stays the sole approver.
# So this script always `exit 1`. Do not change that to 0.
#
# Cleared by the next state-set.sh (working) when the tool runs, or state-end.sh.
set -u
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 1
TOOL=$(printf '%s' "$IN" | jq -r '.tool_name // ""')

# AskUserQuestion raises a permission dialog too, but it is a "pick an answer"
# prompt, not an allow/deny — the PreToolUse:AskUserQuestion hook already set
# "asking" (cs=4). Don't clobber it with "waiting" (cs=2) here, or the device
# shows an Allow/Deny screen for a tool that has neither. Let asking stand;
# exit non-blocking like every other path in this hook.
[ "$TOOL" = "AskUserQuestion" ] && exit 1

# A short human detail to show under the tool name (mirrors what Claude Code's
# own prompt highlights): the command for Bash, the path for file tools, the
# URL for fetches, else the first scalar value in tool_input. Truncated so it
# fits the device label.
DETAIL=$(printf '%s' "$IN" | jq -r '
  .tool_input as $i
  | ( $i.command            # Bash
    // $i.file_path          # Write / Edit / Read / NotebookEdit
    // $i.path
    // $i.url                # WebFetch
    // $i.pattern            # Grep / Glob
    // ( $i | to_entries | map(select(.value | type == "string")) | (.[0].value // "") )
    // "" )
  | tostring | gsub("\n";" ") | .[0:60]
')

DIR="$HOME/.config/claude-usage-monitor/state"
mkdir -p "$DIR"
TMP="$DIR/$SID.json.tmp"
jq -nc --arg t "$TOOL" --arg d "$DETAIL" --arg sid "$SID" --argjson ts "$(date +%s)" \
  '{state:"waiting", tool:$t, detail:$d, ts:$ts, sid:$sid}' > "$TMP" \
  && mv -f "$TMP" "$DIR/$SID.json"
exit 1   # non-blocking "no opinion" — native prompt stays the sole approver
