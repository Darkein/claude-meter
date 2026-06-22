# Audit — claude-meter

**Date:** 2026-06-13
**Scope:** firmware (ESP32 C++), host daemons (bash/python/windows), hooks, tools, build, docs.
**Method:** three parallel code sweeps, then manual verification of every high-impact and uncertain
finding against source. Findings that didn't survive verification are listed under *Filtered out* so
they're not re-chased.

This is a living reference. Part A is the feature inventory; Part B is the prioritized backlog. As
items are fixed, strike them through and link the commit/PR.

---

## Part A — Feature inventory

### Firmware (ESP32, shared + HAL + boards)

- **Boot/setup** ([main.cpp:187](../firmware/src/main.cpp)): board_init → display → idle/brightness →
  power/imu/touch/audio → LVGL (partial render, dual buffer) → BLE → UI → SCREEN_USAGE.
- **Main loop** ([main.cpp:286](../firmware/src/main.cpp)): idle_tick, lv_timer_handler, ui_tick_anim,
  ble_tick, audio_tick, power_tick, imu_tick, display_tick (skipped while asleep), button handling,
  BLE state→UI, battery→UI, serial cmd, BLE data→parse→ui_update→ack/nack. `delay(5)`.
- **Idle/sleep** (`idle.*`, `idle_cfg.h`): 4-state machine AWAKE/FADING_OUT/ASLEEP/FADING_IN. Timeout
  60s, fade-out 400ms, fade-in 180ms. `IDLE_SLEEP_WHEN_CHARGING=false`, `IDLE_WAKE_ON_TOUCH=true` (first
  touch swallowed). Wake-press consumed via `idle_consume_wake_press()`.
- **Hold-to-pair gesture** ([main.cpp:255](../firmware/src/main.cpp)): PWR long-press → PENDING(1.5s) →
  ARMED(3s, release clears bonds + re-advertise) → DISARMED(6s, toward power-off). Clear-on-release is
  deliberate (a power-off hold never releases before the AXP 8s shutdown).
- **Brightness** (`brightness.*`): 4 levels [64,128,200,255], NVS-persisted, applied via idle.
- **Volume** (`volume.*`): 4 levels 0–3, NVS-persisted, confirmation chime on change (C6 only).
- **HAL layer** (`hal/*.h`): display, touch, input, power, imu, audio, board_caps — board-agnostic
  contract; shared code has zero `#ifdef BOARD_*`.
- **Boards**:
  - `waveshare_amoled_216` — S3, 480×480, CO5300, CST9220, AXP2101, QMI8658 rotation, 2 buttons.
  - `waveshare_amoled_18` — S3, 368×448, **2 revs auto-detected** (SH8601+FT3168 / CO5300+CST816),
    XCA9554 IO expander, no rotation, 1 button.
  - `waveshare_amoled_216_c6` — C6, 480×480, **no PSRAM**, ES8311 audio + chimes, no rotation.
  - `template/` — porting skeleton.
- **Software rotation** (216 only, `display.cpp`): CPU pixel remap in 480×40 strips, brightness ramp on
  rotation change.
- **Audio/chimes** (C6, `audio.cpp`): ES8311 + I2S, on-the-fly sine synthesis, ALERT (rising) on
  WAITING/QUESTION, DONE (falling) on work-finish, silence injection to avoid DMA underrun.

### BLE / data (`ble.*`, `data.h`)

- NimBLE peripheral "Claude Meter", service `4c41555a-…0001`, max 2 connections.
- RX(0002) host→device JSON; TX(0003) device ack/nack notify; REQ(0004) device-initiated refresh on
  subscribe when no data yet.
- State machine INIT/ADVERTISING/CONNECTED/DISCONNECTED; re-advertises to fill the 2nd slot (OS HID +
  daemon).
- Bond management: `ble_clear_bonds()` from the pair gesture.
- `UsageData`: session/weekly %+reset, status, claude_state (idle/working/waiting/question/none),
  approval queue (count, sid, tool, detail).

### UI (`ui.*`, `splash.*`, `icons.h`, `logo.h`)

- **2 screens**: SCREEN_USAGE (dual usage bars + reset countdowns + status line + battery/volume/logo),
  SCREEN_APPROVAL (remote permission prompt, tool+detail, "N / M" queue badge, auto-raised).
- **Responsive layout** (`compute_layout()`): breakpoint H≥460 (large) vs compact.
- **3 view sub-states** in USAGE: pairing hint (disconnected) / idle Zzz (data stale >90s,
  `DATA_FRESH_MS`, [ui.cpp:144](../firmware/src/ui.cpp)) / live usage.
- **Status-line animation**: spinner + rotating verbs during work.
- **Splash engine**: 13× 20×20 pixel-art animations, per-anim 10-color palette, runtime scaling, PSRAM
  canvas (internal SRAM fallback on C6).
- **Icons**: 5 battery + 4 volume RGB565A8 (alpha), 80×80 logo.

### Host daemon + tooling

- **`claude_usage_daemon.py`** (primary, macOS/Linux): token from Keychain / `.credentials.json`,
  Anthropic poll (haiku dummy call, rate-limit headers → payload), CoreBluetooth connected-peripheral
  recovery, file-watch of hook state, payload dedup, single-instance flock, async backoff loop.
- **`claude_usage_daemon_windows.py`**: bleak + tray + autostart.
- **`claude-meter.sh`** (legacy Linux/D-Bus): bluetoothctl discovery, MAC cache, busctl GATT
  write, dbus-monitor REQ subscribe.

#### Claude Code live-state pipeline (headline feature — terminal activity → device screen)

- **4 hooks** (`daemon/hooks/*.sh`), each reads hook JSON on stdin and writes an atomic per-session
  `~/.config/claude-meter/state/<sid>.json` (`jq … > tmp && mv`):
  - `state-set.sh <state>` — parametrised working/idle/asking writer (turn start, tool end, MCP
    elicitation answered, AskUserQuestion PreToolUse → asking).
  - `state-perm.sh` — **PermissionRequest** → `{state:waiting, tool, detail, ts}`. **Always `exit 1`**
    (observe-only: exit 0 would AUTO-ALLOW, exit 2 would block — human stays sole approver). Extracts a
    human detail (command/path/url/pattern/first scalar, truncated 60). Skips AskUserQuestion (lets
    `asking` stand).
  - `state-pretool.sh` — **PreToolUse** (after a prompt is granted, before the tool runs) → `working`,
    clearing the device approval screen at decision time, not at slow PostToolUse. Skips AskUserQuestion.
  - `state-end.sh` — **SessionEnd** → `rm` the session file.
- **Daemon derivation** ([`_claude_state_fields`](../daemon/claude_usage_daemon.py)): disk is the **sole
  source of truth**, re-derived every read → restart-safe queue. Output `cs` (0 idle / 1 working / 2
  permission pending / 3 none / 4 question), `aq` queue length, `as` front sid, `tn`/`td` front
  tool/detail. Priority permission(2) > question(4) > working(1) > idle(0). Waiting queue FIFO by ts.
- **Auto-grant / ghost filtering** (the subtle core, churned by both recent commits): `STATE_STALE_S=120`
  (ignore crashed-session working/idle), `DIALOG_MIN_AGE_S=2.5` (skip too-young `waiting` — an acceptEdits
  auto-grant writes `waiting` then PreToolUse overwrites it ~instantly; stops the "approval flashes ~1s
  every ~30s" bug), `WAITING_STABLE_READS=2` re-arm guard (a real blocked prompt FREEZES the loop so its
  ts holds steady; an auto-grant stream keeps advancing ts → treated as working), `DIALOG_STALE_S=180`
  (delete ESC-dismissed/abandoned dialogs — **ESC fires no hook**, so this timeout is the only event-free
  clear).
- **Full hook-event wiring** (`install-hooks.sh`, additive merge into `~/.claude/settings.json`): working
  ← UserPromptSubmit, PreToolUse(* except AskUserQuestion), PostToolUse, PostToolUseFailure,
  ElicitationResult; asking ← PreToolUse:AskUserQuestion, Notification:elicitation_dialog; waiting ←
  PermissionRequest(*); idle ← Stop, StopFailure, Notification:idle_prompt; remove ← SessionEnd. So
  idle/working are **explicit signals**, not inferred — `STATE_STALE_S` is only a crashed-session
  backstop. PermissionRequest is used over Notification:permission_prompt because the latter only fires
  when the window is unfocused.
- **Firmware consumer** (`ui.cpp`): cs=2 raises SCREEN_APPROVAL with tool/detail + "N / M" queue badge;
  pair-screen title flips "Waiting for host" (bonds present) vs "Pairing…" (no bonds) on bond change.
  Firmware also has `ble_send_approval(sid, approve)` ([ble.cpp:198](../firmware/src/ble.cpp)) that
  notifies an Allow/Deny back over TX — **but the daemon never consumes it** (observe-only by design).
  Half-built remote-approval (see I3).
- **Parity gap**: the legacy bash daemon (`claude-meter.sh`) has **no live-state pipeline at all**
  — usage only (`{s,sr,w,wr,st,ok}`). Linux users on it silently get no approval screen / working-idle
  states. The Python daemon (runs on Linux too via bleak) is the only full implementation.

#### Tools / installers

- **Tools** (`tools/*.js`): png_to_lvgl, scrape_claudepix, convert_to_c. **`screenshot.sh`** serial
  framebuffer capture.
- **Installers**: install.sh (systemd), install-mac.sh (LaunchAgent), install-hooks.sh (additive merge).

### Build (`platformio.ini`)

- 3 envs (216 / 18 / 216_c6), pioarduino platform pinned 55.03.38-1, per-env `build_src_filter`,
  qio_opi PSRAM on S3, C6 has no PSRAM + `LV_USE_SNAPSHOT=0` (screenshot disabled).

---

## Resolution log — 2026-06-13 fix pass

All actionable items fixed; firmware builds on all 3 envs, scripts syntax-checked, I2 behavior unit-tested.

| Item | Status | Note |
|------|--------|------|
| P1.1 LVGL buffer null-check | ✅ fixed | `main.cpp` — fail-loud on null buf1/buf2 |
| P1.2 token / 401 recovery | ✅ fixed | Token already re-read every poll → auto-heals; added actionable 401/403 log |
| P2.3 ESC ghost | ✅ fixed via I2 | explicit clear, not timeout tuning (per decision) |
| P2.4 raise latency | ⏸ left | timing untouched by decision (regression risk) |
| P2.5 `_waiting_seen` not persisted | ✅ noted | caveat comment added next to "cannot desync" |
| P2.6 touch ISR race | ❌ false positive | `touch_x/y/pressed` are single-writer (main loop); ISR only sets the flag |
| P2.7 board-rev probe retry | ✅ fixed | 5× retry + probe both addresses + ambiguity log |
| P2.8 io-expander writes unchecked | ✅ fixed | output writes checked, logs + returns false |
| I1 raise latency tuning | ⏸ not done | same as P2.4 |
| I2 explicit dialog clear | ✅ done | `state-set.sh` deletes a waiting/asking file on an idle signal |
| I3 remote approval | ✅ deleted | `ble_send_approval` + TX path removed (dead code) |
| I4 legacy bash daemon | ✅ deleted | `claude-meter.sh` removed; `install.sh` now provisions the Python daemon |
| I5 event channel | ❌ rejected | keep disk-as-truth |
| I6 richer payload | ⏸ not done | product direction |
| P3 stale docs | ✅ fixed | README device name, ui.h/data.h comments, CLAUDE.md daemon section |
| P3 protocol spec | ✅ added | wire-format block in `data.h` |
| P3 approval count cap | ✅ fixed | display capped at "99+" |
| P3 layout magic numbers | ⏸ left | note only, out of scope |

## Part B — Backlog (verified, prioritized)

### P1 — fix soon

1. **LVGL frame buffers not null-checked** — [main.cpp:211-218](../firmware/src/main.cpp).
   `buf1/buf2 = heap_caps_malloc(...)` used in `lv_display_set_buffers` with no null guard. Highest risk
   on **C6** (no PSRAM, 480×480 from internal SRAM). OOM → crash on first render, indistinguishable from a
   hang. Fix: check both, hang with a serial diagnostic.
2. **Daemon never recovers from expired token** — [claude_usage_daemon.py:307-309](../daemon/claude_usage_daemon.py).
   `poll_api` logs `API HTTP 401` and returns None forever; the token arg isn't re-read on auth failure.
   Expired OAuth = silent dead daemon until manual restart. Fix: on 401/403, re-read token (Keychain/file)
   and retry once before backoff. (Verify whether the token is already re-read each outer loop; if so,
   downgrade.)

### P2 — robustness

3. **Pipeline — ESC-dismissed dialog ghost** — `_claude_state_fields`. ESC on a permission/AskUserQuestion
   dialog fires **no hook**. It's cleared by the user's next `UserPromptSubmit` (→ working overwrites the
   file), but if the user ESCs and walks away the screen lingers until the `DIALOG_STALE_S=180s` prune —
   up to 3 min on an abandoned session. Low-effort win: shorten the prune (e.g. 60s) since
   Stop/UserPromptSubmit are the real clears and 180s is only the walk-away backstop. Full fix: I2.
4. **Pipeline — approval-screen raise latency** — same function. A genuine prompt must clear
   `DIALOG_MIN_AGE_S` (2.5s) **and** hold a steady ts across `WAITING_STABLE_READS` (2) reads before
   SCREEN_APPROVAL raises. Inherent cost of the auto-grant-flash fix; device lags the terminal by several
   seconds. Tuning knob, not a bug. See I1.
5. **Pipeline — `_waiting_seen` not persisted** — in-memory dict. The "disk is sole truth, cannot desync"
   comment doesn't cover the stable-read bookkeeping, which lives only in RAM, so a daemon restart
   re-incurs the WAITING_STABLE_READS delay for an in-flight prompt. Minor, self-healing; note the caveat
   or persist the counter.
6. **Touch coord read not atomic vs ISR** — `boards/*/touch.cpp`. Flag cleared then x/y/pressed read as 3
   separate volatiles; ISR can fire mid-read → mismatched coords (ghost touch). Low probability on ESP32
   (aligned reads) but real. Fix: snapshot under a brief critical section.
7. **AMOLED-1.8 board-rev probe has no retry** — [board_init.cpp](../firmware/src/boards/waveshare_amoled_18/board_init.cpp).
   A noisy I2C bus at boot → wrong revision → wrong display/touch driver → silent black screen. Fix: probe
   both touch addresses, retry a few times, log on ambiguity.
8. **IO expander writes unchecked** — [io_expander.cpp](../firmware/src/boards/waveshare_amoled_18/io_expander.cpp).
   If config/reset writes fail, display+touch stay in reset (black screen) with no diagnostic. Fix:
   propagate bool, log on failure.

### Live-state pipeline — improvement roadmap (functional, beyond bug-fixes)

The pipeline leans on inference heuristics (age windows, stable-read counts, staleness prunes) because
Claude Code gives no explicit "dialog dismissed" / "this is a real block" signal. These reduce that
reliance or extend the feature.

- **I1 — Cut the approval-raise latency.** Today a genuine prompt waits `DIALOG_MIN_AGE_S` (2.5s) + a
  steady ts across `WAITING_STABLE_READS` (2) polls — the price of telling a real block apart from an
  acceptEdits auto-grant transient. Lower it by tightening the waiting-check cadence, or by treating a
  `waiting` file as real the instant one short tick passes with no PreToolUse(working) overwrite (the
  auto-grant always overwrites fast). Measure against the "flash every ~30s" regression the current values
  fixed before lowering them.
- **I2 — Kill the ESC ghost properly.** Add a clear-on-resolve path so a dismissed dialog disappears
  immediately instead of lingering to the 180s prune: a lightweight hook on the next user turn that
  deletes the session's `waiting`/`asking` file, and/or a shorter `DIALOG_STALE_S`. (Backlog #3 is the
  quick version; this is the complete one.)
- **I3 — Wire up remote Allow/Deny (the half-built `ble_send_approval`).** Biggest functional upgrade:
  approve/deny tools from the device. Hard part: a PermissionRequest/PreToolUse hook is a one-shot
  synchronous process, and a *blocking* PreToolUse strands the session (install notes warn this
  explicitly), so it must block-with-timeout on a decision file the daemon writes when the device's BLE
  approval notify arrives, then fall back to the native prompt on timeout. **Security-sensitive** — a desk
  gadget approving arbitrary tool execution means anyone in physical proximity can approve; gate behind
  explicit opt-in + short timeout. **Decision needed:** build it, or delete `ble_send_approval` + the TX
  approval path as dead code (YAGNI) until it's pursued.
- **I4 — Retire the legacy bash daemon.** It carries no live-state pipeline, so Linux users on it silently
  lose the whole feature (parity gap above). The Python daemon already runs on Linux (bleak). Consolidate
  to one daemon and mark/remove the bash one.
- **I5 — Event channel vs disk-poll (considered → likely reject).** A local socket/FIFO would give ordered
  events and remove the per-tick glob+read+parse and all the age math, but loses the restart-safe
  "disk = sole truth" property (a genuine strength here) and adds a listener dependency for a handful of
  sessions. Keep the file model unless profiling shows the poll cost matters; recorded as considered.
- **I6 — Richer state payload (optional product direction).** The channel already carries tool/detail;
  could add current model, turn-elapsed, project/cwd, or cost-burn so the device shows more than usage +
  approval.

### P3 — polish / docs

- **Stale docs**: README + `ui.h` comments reference a "Bluetooth screen" that no longer exists (UI
  collapsed to USAGE+APPROVAL); README missing the C6 board in quick-start. Fix copy.
- **No daemon↔device protocol spec**: `data.h` documents the struct but there's no versioned JSON schema
  doc for the BLE payload. Add a short protocol section (fields `s/sr/w/wr/st/ok/cs/aq/as/tn/td`).
- **`approval_count` display unbounded** — `data.h` int, rendered as "N / M". Cap display at 99.
- **Layout magic numbers hardcoded** — `ui.cpp` margins/positions inline; only 2 breakpoints. Known
  limitation for custom-board porters (partly acknowledged in porting docs).
- **Legacy bash daemon lacks `set -euo pipefail`** — `claude-meter.sh:1`. Low priority (python
  daemon is primary); harden or mark deprecated if still shipped (see I4).

---

## Filtered out (explorer false positives — do NOT chase)

- ~~Bash `find_char_path_by_uuid` subshell loses output~~ — **false**: `echo` propagates through function
  stdout to `req_path=$(…)` at `claude-meter.sh:148`. Works.
- ~~Bash `$bytes` unquoted = shell injection~~ — **false**: intentional word-split for the `ay` byte array;
  bytes are always `0xNN`, no metacharacters. Quoting would break it.
- ~~BLE `memcpy(rx_buf, val.c_str(), len)` overflow~~ — **false**: `std::string` is null-terminated and
  `len ≤ length()`. `ble.cpp:79-81` is safe.
- ~~idle `millis()` wraparound bug~~ — **false**: unsigned subtraction is the canonical wrap-safe idiom.
- ~~`DATA_FRESH_MS` undefined / 30s~~ — **false**: defined `ui.cpp:144` = 90000 (90s).
- ~~onConnect BLE state race~~ — **false**: `start_advertising` (`ble.cpp:44`) guards UI state correctly.
- ~~Hook `jq … > "$TMP" && mv` doesn't check jq exit~~ — **false**: `A && B` runs `mv` only if `jq` exits
  0, so a jq failure leaves the previous state file intact (a leftover `.tmp` is harmless, overwritten
  next time). The hooks are safe.
