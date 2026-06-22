# OTA firmware updates (BLE)

The daemon can push a new firmware image to the device over the existing BLE
link — no USB cable. The receiver is shared code, so every board with a
dual-OTA partition layout supports it.

## What's where

| Piece | File |
|-------|------|
| OTA receiver (Update + SHA-256) | `firmware/src/ota.{h,cpp}` |
| GATT channel + board/version chars | `firmware/src/ble.cpp` (`OtaCallbacks`, `INFO_CHAR`) |
| Progress overlay | `firmware/src/ui.cpp` (`ui_ota_show`, `ui_ota_set_pct`) |
| Finalize + reboot wiring | `firmware/src/main.cpp` (`ota_tick`) |
| Host pusher | `daemon/ota_push.py`, `python -m daemon --ota` |
| Build-then-push helper | `tools/build_and_ota.sh` |
| Auto version stamping | `firmware/tools/gen_version.py` |

## Wire protocol

Custom data service `4c41555a-…0001`:

- `…0005` **OTA_CHAR** (WRITE, with response) — host writes one frame per write.
  Each frame is a 1-byte opcode + payload:
  | Opcode | Frame |
  |--------|-------|
  | `0x01` BEGIN | + `u32 LE` total size + `32 B` SHA-256 of the whole image |
  | `0x02` DATA  | + raw image bytes, in order |
  | `0x03` END   | finalize: device verifies SHA-256, commits, reboots |
  | `0x04` ABORT | cancel the in-progress transfer |

  The opcode prefix on DATA is what lets image bytes that happen to start with
  `0x01`/`0x03`/`0x04` not be mistaken for control frames.

- `…0003` **TX_CHAR** (NOTIFY) — device reports the terminal result:
  `{"ota":"done"}` or `{"ota":"err","c":<code>}`.
  Error codes (`ota.h`): 1 begin, 2 write, 3 SHA mismatch, 4 end/commit.

- `…0006` **INFO_CHAR** (READ) — `{"board":"<id>","fw":"<version>","git":"<sha>"}`.
  The host reads it before pushing to refuse a wrong-board image, and after the
  reboot to confirm the new version. `board` is `board_caps().id` (the
  PlatformIO env name).

`WRITE` with response is used deliberately: each chunk's ACK paces the stream
against the device's flash writes (built-in flow control), and the BEGIN write
naturally waits while the device erases the OTA partition.

## Safety

- **SHA-256 verified before commit.** A corrupt transfer never flips the boot
  partition; the device stays on the current firmware.
- **Power-loss safe.** `Update.end(true)` switches the boot partition only after
  a full, verified write. `otadata` keeps the old app bootable.
- **Bonding gates writes.** Only the paired host can push (`setSecurityAuth`).
- Not yet: signed images / secure boot. Add later for stronger trust.

## Partition requirement

OTA needs a dual-app layout (`app0`/`app1` + `otadata`). Status by board:

| Board env | Layout | OTA-ready out of the box? |
|-----------|--------|---------------------------|
| `waveshare_amoled_216_c6` | `default_16MB.csv` | ✅ |
| `waveshare_amoled_18` | `default_16MB.csv` | ✅ |
| `waveshare_amoled_216` | `default_16MB.csv` (newly set) | ⚠️ needs one wired reflash first |

OTA cannot migrate its own partition table, so a 2.16 already running the old
single-app layout needs **one** USB `erase + flash` to adopt the dual-OTA
layout. After that it updates over BLE like the others.

> First OTA-capable firmware always goes over USB once per board — the currently
> running firmware has no receiver. From the next flash onward it's BLE-only.

## Pushing an update

The pusher is a **one-shot** command and connects itself, so **stop the running
daemon first** (it holds the single-instance lock + a BLE connection slot):

```bash
# macOS
launchctl stop com.claude.usage-monitor      # or: kill the daemon
# Linux
systemctl --user stop claude-meter
```

Then build + push in one step:

```bash
./tools/build_and_ota.sh waveshare_amoled_216_c6
```

or manually:

```bash
pio run -d firmware -e waveshare_amoled_216_c6
python -m daemon --ota firmware/.pio/build/waveshare_amoled_216_c6/firmware.bin \
                 --board waveshare_amoled_216_c6
```

`--board` is the guard: the push aborts if the connected device reports a
different `board` id. Restart the daemon when done.

> **macOS Bluetooth permission.** The push uses CoreBluetooth, which needs the
> Bluetooth TCC permission for the *responsible app* (your terminal). Run it
> from a real Terminal.app / iTerm session, not from a sandboxed/automation
> context — an unauthorized process crashes immediately with `SIGABRT` and
> `"...NSBluetoothAlwaysUsageDescription..."`. If your terminal crashes too,
> grant it Bluetooth in System Settings → Privacy & Security → Bluetooth. The
> launchd-managed daemon already has this; a bare CLI run may not.

Throughput is MTU-bound: BlueZ negotiates ~517 (≈513 B/chunk, ~1–2 min for a
~1.6 MB image); macOS caps ~185 (≈181 B/chunk, slower). The host adapts
automatically from `client.mtu_size`.

## Auto version numbering

`firmware/tools/gen_version.py` (a PlatformIO `pre:` script on every env) stamps
`FW_VERSION` into `firmware/src/version_gen.h` before each build:

- a new git branch claims the next **minor** (first build on it),
- every build bumps the **patch**,
- **major** is manual (edit `firmware/.fw_version.json`).

Both `.fw_version.json` and `version_gen.h` are gitignored (local to the build
machine). A generated header is used instead of a `-D` macro so a changing
version doesn't bust the whole build cache — only files including `version.h`
recompile.

## Pushing automatically after every build (optional)

`tools/build_and_ota.sh` already does build→push in one command. To make a bare
`pio run` also push, add a PlatformIO **post-build** hook to the env (opt-in, so
normal builds stay offline):

```ini
; in the [env:…] block
extra_scripts =
    pre:tools/gen_version.py
    post:tools/ota_after_build.py
```

`tools/ota_after_build.py` would shell out to
`python -m daemon --ota $BUILD_DIR/firmware.bin --board $PIOENV` from an
`env.AddPostAction("buildprog", …)` hook. Gate it behind an env var (e.g.
`OTA_ON_BUILD=1`) so CI / offline builds don't try to reach a device.
```
