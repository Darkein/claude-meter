#!/bin/bash
# Install the Clawdmeter Claude Code hooks: copy the hook scripts into the
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
# Session-state event map (no blind spots):
#   working  <- UserPromptSubmit, PostToolUse, PostToolUseFailure, ElicitationResult
#   asking   <- PreToolUse:AskUserQuestion, Notification:elicitation_dialog
#   waiting  <- PermissionRequest (every tool-permission dialog; state-perm.sh)
#   idle     <- Stop, StopFailure, Notification:idle_prompt
#   (remove) <- SessionEnd
#
# "asking" and "waiting" both clear automatically: once the user answers, the
# agentic loop resumes and the next PostToolUse fires "working", overwriting it
# (on a denied permission, no tool runs and Stop -> idle clears it instead).
#
# PermissionRequest is used for permission dialogs instead of
# Notification:permission_prompt because the Notification only fires when the OS
# emits a banner (window unfocused); PermissionRequest fires for every dialog.
# state-perm.sh is observe-only: it records state then exits 1 (non-blocking) so
# the native prompt remains the sole approver. It must NEVER exit 0 (= auto-allow).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"        # .../daemon
HOOK_SRC="$SCRIPT_DIR/hooks"
HOOK_DST="$HOME/.config/claude-usage-monitor/hooks"
SETTINGS="$HOME/.claude/settings.json"

command -v jq >/dev/null || { echo "Error: jq is required to merge settings.json"; exit 1; }

echo "  Copying hook scripts -> $HOOK_DST"
mkdir -p "$HOOK_DST"
cp "$HOOK_SRC"/state-set.sh "$HOOK_SRC"/state-perm.sh "$HOOK_SRC"/state-end.sh "$HOOK_DST/"
chmod +x "$HOOK_DST"/*.sh
# Drop scripts from the previous install layout so they can't linger on disk.
rm -f "$HOOK_DST"/state-working.sh "$HOOK_DST"/state-idle.sh "$HOOK_DST"/state-approve.sh \
      "$HOOK_DST"/state-waiting.sh

WORK="$HOOK_DST/state-set.sh working"
IDLE="$HOOK_DST/state-set.sh idle"
ASK="$HOOK_DST/state-set.sh asking"
PERM="$HOOK_DST/state-perm.sh"
END="$HOOK_DST/state-end.sh"

mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
cp "$SETTINGS" "$SETTINGS.clawdmeter.bak"
echo "  Backed up settings.json -> $SETTINGS.clawdmeter.bak"

# Additive merge. strip() removes any prior Clawdmeter entry (current OR legacy
# script paths) from a group, leaving third-party hooks (e.g. sound) untouched,
# then we re-add our entries. clean() drops a now-empty event group entirely.
jq \
  --arg work "$WORK" --arg idle "$IDLE" --arg ask "$ASK" --arg perm "$PERM" --arg end "$END" \
  --arg legacy_dir "$HOOK_DST" \
  '
  # true if a hook command is one of ours (current args or any legacy script)
  def is_ours($cmd):
    ($cmd == $work) or ($cmd == $idle) or ($cmd == $ask) or ($cmd == $perm) or ($cmd == $end)
    or ($cmd | test("/state-(working|idle|approve|set|waiting|perm|end)\\.sh"));

  def strip:
    map(.hooks |= map(select(is_ours(.command) | not)))
    | map(select((.hooks | length) > 0));

  def clean($k):
    if (.hooks[$k] | length // 0) == 0 then del(.hooks[$k]) else . end;

  # PreToolUse: NO approval gate (display-only). Strip any prior entry, then add
  # one matcher for AskUserQuestion -> "asking" (the only hook that fires when the
  # built-in question tool blocks for a user choice; no Notification is emitted).
  .hooks.PreToolUse = ((.hooks.PreToolUse // []) | strip)
    + [ {matcher:"AskUserQuestion", hooks:[ {type:"command", command:$ask} ]} ]

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
    + [ {matcher:"", hooks:[ {type:"command", command:$work} ]} ]

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
    + [ {matcher:"elicitation_dialog", hooks:[ {type:"command", command:$ask} ]} ]

  # --- cleanup ---
  | .hooks.SessionEnd = ((.hooks.SessionEnd // []) | strip)
    + [ {matcher:"", hooks:[ {type:"command", command:$end} ]} ]
  ' \
  "$SETTINGS.clawdmeter.bak" > "$SETTINGS"

echo "  Merged Clawdmeter hooks into $SETTINGS"
