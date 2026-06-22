#!/bin/bash
# Install the Claude Meter Claude Code hooks: copy the hook scripts into the
# daemon config dir and additively merge the hook entries into
# ~/.claude/settings.json (existing hooks are preserved). Idempotent, and
# upgrades cleanly from older installs (drops the previous script set first).
#
# Called by install.sh / install-mac.sh, but safe to run standalone.
#
# Design: every hook is display-only and NON-BLOCKING. We do NOT install a
# PreToolUse approval gate — a blocking PreToolUse hook strands the whole session
# whenever the daemon/device can't answer, and Claude Code only supports
# --permission-prompt-tool in non-interactive (-p) mode anyway. The native
# Claude Code prompt stays the sole approver; the device only mirrors state.
#
# Each per-session state file carries TWO independent facts so they never clobber
# each other (the daemon gives "dialog" priority over "activity"):
#   activity : working | idle            — turn progress
#   dialog   : none | waiting | asking   — pending UI (raises the device screen)
#
# Session-state event map (no blind spots):
#   activity=working <- UserPromptSubmit, PreToolUse:* (except AUQ/ExitPlanMode),
#                       PostToolUse, PostToolUseFailure, ElicitationResult accept.
#                       Also CLEARS dialog (a tool running / answer submitted = resolved).
#   activity=idle    <- Stop, StopFailure, Notification:idle_prompt.
#                       Sets activity ONLY — never clears an "asking" dialog (see below).
#   dialog=asking    <- PreToolUse:AskUserQuestion (kind=question),
#                       PreToolUse:ExitPlanMode (kind=plan),
#                       Notification:elicitation_dialog (kind=elicitation)
#   dialog=waiting   <- PermissionRequest (every tool-permission dialog EXCEPT
#                       AUQ/ExitPlanMode, which state-perm.sh skips)
#   dialog=none      <- ElicitationResult decline/cancel (explicit dismissal),
#                       plus every activity=working event above.
#   (remove file)    <- SessionEnd
#
# WHY a bare idle must NOT clear an "asking" dialog (the bug this fixes): opening
# an AskUserQuestion/ExitPlanMode dialog PAUSES the turn, and Claude Code emits a
# Stop/idle_prompt while the dialog is still open. The old design read that idle
# as "dialog resolved" and deleted it, so the device showed "idle" for the whole
# question. Now idle sets activity only; "asking" clears on the answer (PostToolUse
# -> working), an explicit dismissal, SessionEnd, or the daemon's short dialog TTL.
#
# Asymmetry — idle DOES clear "waiting": a permission dialog being open emits no
# Stop (verified), so an idle while dialog=waiting can only be a DENY/ESC with no
# tool run — its legitimate clear signal. A granted permission instead fires
# PostToolUse -> working. ESC-abandon of either dialog fires no hook; the daemon's
# DIALOG_STALE_S TTL is the event-free backstop (a new prompt clears it instantly).
#
# PermissionRequest is used for permission dialogs instead of
# Notification:permission_prompt because the Notification only fires when the OS
# emits a banner (window unfocused); PermissionRequest fires for every dialog.
# state-perm.sh is observe-only: it records state then exits 1 (non-blocking) so
# the native prompt remains the sole approver. It must NEVER exit 0 (= auto-allow).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"        # .../daemon
HOOK_SRC="$SCRIPT_DIR/hooks"
HOOK_DST="$HOME/.config/claude-meter/hooks"
SETTINGS="$HOME/.claude/settings.json"

command -v jq >/dev/null || { echo "Error: jq is required to merge settings.json"; exit 1; }

echo "  Copying hook scripts -> $HOOK_DST"
mkdir -p "$HOOK_DST"
cp "$HOOK_SRC"/state-set.sh "$HOOK_SRC"/state-perm.sh "$HOOK_SRC"/state-pretool.sh "$HOOK_SRC"/state-elicit.sh "$HOOK_SRC"/state-end.sh "$HOOK_DST/"
chmod +x "$HOOK_DST"/*.sh
# Drop scripts from the previous install layout so they can't linger on disk.
rm -f "$HOOK_DST"/state-working.sh "$HOOK_DST"/state-idle.sh "$HOOK_DST"/state-approve.sh \
      "$HOOK_DST"/state-waiting.sh

WORK="$HOOK_DST/state-set.sh working"
IDLE="$HOOK_DST/state-set.sh idle"
ASK_Q="$HOOK_DST/state-set.sh asking question"
ASK_PLAN="$HOOK_DST/state-set.sh asking plan"
ASK_ELICIT="$HOOK_DST/state-set.sh asking elicitation"
PERM="$HOOK_DST/state-perm.sh"
PRETOOL="$HOOK_DST/state-pretool.sh"
ELICIT="$HOOK_DST/state-elicit.sh"
END="$HOOK_DST/state-end.sh"

mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
cp "$SETTINGS" "$SETTINGS.claude-meter.bak"
echo "  Backed up settings.json -> $SETTINGS.claude-meter.bak"

# Additive merge. strip() removes any prior Claude Meter entry (current OR legacy
# script paths) from a group, leaving third-party hooks (e.g. sound) untouched,
# then we re-add our entries. clean() drops a now-empty event group entirely.
jq \
  --arg work "$WORK" --arg idle "$IDLE" --arg perm "$PERM" \
  --arg ask_q "$ASK_Q" --arg ask_plan "$ASK_PLAN" --arg ask_elicit "$ASK_ELICIT" \
  --arg pretool "$PRETOOL" --arg elicit "$ELICIT" --arg end "$END" \
  --arg legacy_dir "$HOOK_DST" \
  '
  # true if a hook command is one of ours (current args or any legacy script)
  def is_ours($cmd):
    ($cmd == $work) or ($cmd == $idle) or ($cmd == $perm)
    or ($cmd == $ask_q) or ($cmd == $ask_plan) or ($cmd == $ask_elicit)
    or ($cmd == $pretool) or ($cmd == $elicit) or ($cmd == $end)
    or ($cmd | test("/state-(working|idle|approve|set|waiting|perm|pretool|elicit|end)\\.sh"));

  def strip:
    map(.hooks |= map(select(is_ours(.command) | not)))
    | map(select((.hooks | length) > 0));

  def clean($k):
    if (.hooks[$k] | length // 0) == 0 then del(.hooks[$k]) else . end;

  # PreToolUse: NO approval gate (display-only). Strip any prior entry, then add:
  #   - AskUserQuestion -> dialog=asking, kind=question (blocked on a choice)
  #   - ExitPlanMode    -> dialog=asking, kind=plan (plan-approval prompt; kind lets
  #                        the device show "Approve plan?" vs "Claude is asking").
  #                        asking is reported immediately by the daemon (no stability
  #                        gate — it is never a transient auto-grant).
  #   - *              -> activity=working (a tool is starting; PreToolUse fires
  #                        BEFORE the permission dialog, so this just marks work and
  #                        clears a stale dialog. state-pretool.sh skips
  #                        AskUserQuestion/ExitPlanMode so it never clobbers asking.)
  .hooks.PreToolUse = ((.hooks.PreToolUse // []) | strip)
    + [ {matcher:"AskUserQuestion", hooks:[ {type:"command", command:$ask_q} ]} ]
    + [ {matcher:"ExitPlanMode",    hooks:[ {type:"command", command:$ask_plan} ]} ]
    + [ {matcher:"*",               hooks:[ {type:"command", command:$pretool} ]} ]

  # PermissionRequest fires for EVERY tool permission dialog, focused or not
  # (Notification:permission_prompt only fires when the OS emits a notification,
  # i.e. window unfocused — which is why inline approvals never reached the
  # device). state-perm.sh records "waiting" then exits 1 (non-blocking): the
  # native dialog stays the sole approver. matcher "*" = all tools.
  | .hooks.PermissionRequest = ((.hooks.PermissionRequest // []) | strip)
    + [ {matcher:"*", hooks:[ {type:"command", command:$perm} ]} ]

  # --- working ---
  | .hooks.UserPromptSubmit = ((.hooks.UserPromptSubmit // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$work} ]} ]
  | .hooks.PostToolUse = ((.hooks.PostToolUse // []) | strip)
    + [ {matcher:"*", hooks:[ {type:"command", command:$work} ]} ]
  | .hooks.PostToolUseFailure = ((.hooks.PostToolUseFailure // []) | strip)
    + [ {matcher:"*", hooks:[ {type:"command", command:$work} ]} ]
  | .hooks.ElicitationResult = ((.hooks.ElicitationResult // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$elicit} ]} ]

  # --- idle ---
  | .hooks.Stop = ((.hooks.Stop // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$idle} ]} ]
  | .hooks.StopFailure = ((.hooks.StopFailure // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$idle} ]} ]

  # --- asking / idle on the Notification channel ---
  # elicitation_dialog -> asking (an MCP user choice, like AskUserQuestion).
  # idle_prompt -> idle. Permission prompts are NOT handled here — they go
  # through PermissionRequest (above), which fires reliably regardless of focus.
  | .hooks.Notification = ((.hooks.Notification // []) | strip)
    + [ {matcher:"idle_prompt",        hooks:[ {type:"command", command:$idle} ]} ]
    + [ {matcher:"elicitation_dialog", hooks:[ {type:"command", command:$ask_elicit} ]} ]

  # --- cleanup ---
  | .hooks.SessionEnd = ((.hooks.SessionEnd // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$end} ]} ]
  ' \
  "$SETTINGS.claude-meter.bak" > "$SETTINGS"

echo "  Merged Claude Meter hooks into $SETTINGS"
