#!/bin/bash
# Clawdmeter PermissionRequest hook. Fires whenever Claude Code shows a tool
# permission dialog (Allow / Allow-always / No) — for EVERY prompt, focused or
# not, unlike Notification:permission_prompt which only fires when the OS emits
# a notification (window unfocused). Sets dialog=waiting with the tool name so
# the daemon raises the device's approval screen.
#
# OBSERVE-ONLY. This hook must NEVER decide the permission:
#   - exit 0 with empty stdout would AUTO-ALLOW the tool (PermissionRequest
#     treats exit 0 as behavior:"allow"). We must NOT do that.
#   - exit 2 would block.
#   - any OTHER non-zero code = non-blocking, the native dialog shows normally
#     and the human stays the sole approver.
# So this script always `exit 1`. Do not change that to 0.
#
# Cleared by: PostToolUse (working) when the tool runs after a grant; an idle
# signal (Stop / idle_prompt) on a deny/ESC (see state-set.sh); SessionEnd; or
# the daemon's dialog TTL.
set -u
IN=$(cat)
SID=$(printf '%s' "$IN" | jq -r '.session_id // empty')
[ -z "$SID" ] && exit 1
TOOL=$(printf '%s' "$IN" | jq -r '.tool_name // ""')

# AskUserQuestion and ExitPlanMode raise a dialog too, but they are "pick an
# answer" / "approve a plan" prompts, not allow/deny — their PreToolUse matchers
# already set dialog=asking. Don't clobber that with "waiting" here, or the
# device shows an Allow/Deny screen for a prompt that has neither. Let asking
# stand; exit non-blocking like every other path in this hook.
case "$TOOL" in AskUserQuestion|ExitPlanMode) exit 1 ;; esac

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
FILE="$DIR/$SID.json"
TMP="$FILE.tmp"
EXIST=$(cat "$FILE" 2>/dev/null); [ -z "$EXIST" ] && EXIST='{}'
printf '%s' "$EXIST" | jq -c --arg t "$TOOL" --arg d "$DETAIL" --arg sid "$SID" \
  --argjson ts "$(date +%s)" \
  '. + {dialog:"waiting", kind:"permission", tool:$t, detail:$d, dialog_ts:$ts, sid:$sid}' \
  > "$TMP" && mv -f "$TMP" "$FILE"
exit 1   # non-blocking "no opinion" — native prompt stays the sole approver
