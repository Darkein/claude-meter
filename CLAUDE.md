# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. Each supported
board lives in its own `firmware/src/boards/<name>/` folder and is selected
via PlatformIO's `build_src_filter`. Adding a board means dropping in a new
folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Three reference ports today:

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_216_c6/` — Waveshare **ESP32-C6**-Touch-AMOLED-2.16 (same CO5300 480×480 panel as the S3 2.16, CST9220 touch, ES8311 audio + speaker). Single-core RISC-V @160MHz, Wi-Fi 6, **no PSRAM**, BLE 5.3 only (no classic BT). Build env: `waveshare_amoled_216_c6`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-2.16 (C6 - default board)
Same 480×480 CO5300 panel as the S3 2.16, but a **different SoC and GPIO map**. SoC: **ESP32-C6** — single-core RISC-V @160MHz, Wi-Fi 6 (2.4GHz), **BLE 5.3 only (no classic BT)**, **no PSRAM**, 16MB NOR flash. Onboard chip antenna; IPEX1 external-antenna option via resoldering one resistor. Pins verified against the official Waveshare XiaoZhi BSP.
- Display: **CO5300** AMOLED via QSPI (CS=15, SCLK=0, SDIO0..3=1..4). **No MCU RST GPIO** — LCD_RST is fed by the AXP2101 **ALDO3** panel rail (cycling the rail resets the panel); `Arduino_CO5300` gets `GFX_NOT_DEFINED` for reset. That class's `begin()` covers most init but the panel stays black without the MFR page-0x20 driving-voltage regs (`0x19`/`0x1C`), set in `send_panel_driving_init()` after `begin()`. MADCTL kept at class default 0x00 (USB port on the side = preferred desk orientation).
- Touch: **CST9220** via I2C (SDA=8, SCL=7, INT=5, RST=11, addr=0x5A). Same controller as the S3 2.16 — SensorLib `TouchDrvCST92xx` (CST92xx) drives it. *(the `board.h` comment still calls it "CST9217"; cosmetic — the `CST9220_ADDR` macro and the official GPIO schema both say CST9220.)*
- PMU: **AXP2101** @ 0x34 — battery (MX1.25 2-pin Li-ion header, charge/discharge), USB VBUS, PWR button. The PWR button sits on the AXP **PWRKEY**; firmware reads short/long/release by polling the AXP IRQ status over I2C (`power.cpp`), not via the GPIO18 IRQ line the schema shows. Panel rails: ALDO3 = panel (+ LCD_RST), ALDO2 = panel power-enable + speaker-amp enable (PA_CTRL).
- IMU: **QMI8658** @ 0x6B — present, drives auto-rotation (`has_rotation=1`, `BOARD_HAS_ROTATION 1` in `board.h`). No PSRAM, so the rotation strip (`rot_buf`) lives in internal SRAM; if that alloc fails, `display_hal_draw_bitmap` falls back to an unrotated blit. Polled accelerometer → quadrant (`imu.cpp`); INT1=GPIO16, INT2=GPIO17 wired but unused.
- RTC: **PCF85063** @ 0x51 on the shared I2C bus — keeps wall-clock time offline (`BOARD_HAS_RTC`).
- Audio: **ES8311** codec @ 0x18 (I2C control on the shared bus; I2S MCLK=19, BCLK/SCLK=20, LRCK=22, DOUT/DSDIN=23, DIN/ASDOUT=21) → speaker amp (PA_CTRL = AXP ALDO2) → 2-pin speaker pads. Firmware plays synthesized chimes **DAC-only @16kHz/16-bit/mono**; codec init is hand-rolled (no vendored codec lib, same stance as touch). **ES7210** ADC + dual-mic array (echo-cancel/voice, shares the I2S bus on 21/22/23) and the **micro-SD slot** (SCK=0/MOSI=1/MISO=2 shared with the QSPI display pins, CS=GPIO6; FAT32) are populated on the board but **unused by firmware**.
- Orientation: **IMU auto-rotation** (0/90/180/270 via CPU strip remap in `display_hal_draw_bitmap`). Touch arrives in raw panel coords, so `touch.cpp` un-rotates it back into LVGL's logical frame by the same quadrant (inverse of `rotate_strip`) — without this, all touch (sliders, top-left Settings tap) is misaligned when rotated. Even-aligned flush regions required (CO5300) — `display_hal_round_area`.
- Buttons: **GPIO 9** (BOOT → primary, cycles brightness; **hold while powering on = serial download mode**), **GPIO 10** (KEY → secondary, cycles chime volume), AXP PWRKEY (PWR → cycle screens; hold ~3s + release = pairing; long-press = power off, short-press = power on). All three confirmed on the official GPIO schema. `BOARD_HAS_SECONDARY_BUTTON` = 1, no third GPIO button.

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_216_c6/ — ESP32-C6: CO5300 + CST9220 + AXP + QMI8658 + PCF85063 RTC + ES8311 audio (no PSRAM, BLE-only)
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — 3-screen UI (splash, usage, bluetooth). compute_layout() picks fonts/positions from board_caps() (responsive — current breakpoint: H >= 460 → large, else compact)
  splash.{h,cpp}            — 20×20 pixel-art engine. CELL = min(W,H)/20, centered.
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard
  data.h                    — UsageData struct
  icons.h                   — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8,
`audio.cpp` for the ES8311 codec on the C6).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (default original)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build C6 2.16
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (new port)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
pio run -d firmware -e waveshare_amoled_216_c6 -t upload --upload-port /dev/cu.usbmodem101 # flash C6 on macOS
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. The S3 boards expose native USB-JTAG (no boot-mode dance needed). The **C6 has only USB-Serial-JTAG (HWCDC)** — its env sets both `ARDUINO_USB_CDC_ON_BOOT=1` and `ARDUINO_USB_MODE=1` so `Serial` maps to HWCDCSerial. Asserting **DTR while pulsing reset holds GPIO9 LOW → the C6 drops into serial download mode** instead of running the app; reset it cleanly with DTR=False + an RTS pulse.

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

**C6 uses a different capture path (still via `screenshot.sh`)** — a full-frame RGB565 snapshot (~460 KB) doesn't fit in C6 internal SRAM, so `LV_USE_SNAPSHOT=0` there. Instead `send_screenshot` force-redraws the screen and streams each partial-render strip out the serial port as `my_flush_cb` flushes it; the host concatenates them into the same contiguous RGB565 the S3 snapshot produces, so `screenshot.sh` works on the C6 unchanged.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.
11. **C6 has no PSRAM.** Its env omits `-DBOARD_HAS_PSRAM`; shared code (`main.cpp`, `splash.cpp`) gates on this to allocate LVGL buffers + the splash canvas from `MALLOC_CAP_INTERNAL` and shrink them. `LV_USE_SNAPSHOT=0` (screenshot unsupported). The C6 also needs the 16MB partition layout (`board_build.partitions = default_16MB.csv`) — the default 1.25MB app partition can't hold the ~1.43MB firmware.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- **Project audit (2026-06-13).** Full feature inventory + prioritized stability/improvement backlog in [`docs/audit.md`](docs/audit.md). Living doc — consult before planning fixes. Covers the Claude live-state pipeline in depth (hooks → daemon heuristics → firmware), a remote-approval improvement roadmap, and a list of verified-false findings not to re-chase.
- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added third board port: Waveshare **ESP32-C6**-AMOLED-2.16 (CO5300 + CST9220, no PSRAM, BLE-only). Adds an audio path (ES8311 codec + speaker chimes) and PCF85063 RTC. Internal-SRAM LVGL buffers; screenshot unsupported.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

Python daemon (`daemon/claude_usage_daemon.py`, run via bleak) is the single cross-platform daemon: reads the OAuth token (macOS Keychain / `~/.claude/.credentials.json`), polls the Anthropic API, AND drives the live Claude Code state pipeline from the hook state files. macOS uses launchd (`install-mac.sh`), Linux uses a systemd user unit (`install.sh` → `systemctl --user start claude-usage-daemon`); both render `ExecStart` to `<venv>/bin/python …/claude_usage_daemon.py`. Windows: `claude_usage_daemon_windows.py`. (The old bash daemon was usage-only and has been removed.)

**Discovery & resilience:**

- Connects by name (`"Clawdmeter"`); Linux caches the resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: Linux drops the cached address; macOS skips the stale CoreBluetooth handle one cycle so the scan fallback is reachable. Exponential backoff 1→60s, reset on success.
- Single-instance flock at `~/.config/claude-usage-monitor/daemon.lock` (two daemons would fight over the one BLE link).
- `POLL_INTERVAL=60`, `TICK=5`. `poll_loop` polls when 60s elapsed OR the ESP fires a refresh; `watch_loop` (watchfiles) pushes on every hook-state change.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. The Python daemon subscribes with bleak `start_notify(REQ_CHAR_UUID)`; the callback sets an asyncio event that wakes `poll_loop` for an immediate poll.
