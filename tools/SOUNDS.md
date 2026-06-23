# Notification sound themes — sources & pipeline

The C6 board plays one of four selectable **sound themes** (Settings → Sound
theme). Each theme provides three event sounds:

| Event        | Fires when                              | `audio_sound_t` |
|--------------|------------------------------------------|-----------------|
| `permission` | a tool-permission prompt is pending (cs=2) | `SND_PERMISSION` |
| `ask`        | a plan approval or AskUserQuestion (cs=4)  | `SND_ASK`        |
| `done`       | Claude finished its turn (WORKING→IDLE)    | `SND_DONE`       |

- **Retro** (theme 0) is synthesized in firmware (`audio.cpp`, `CHIME_*`) — no
  samples.
- **Modern / Bells / Arcade** are sampled PCM, embedded in flash via
  `audio_samples.h`.

## Sources (all CC0 / public domain)

All sampled clips come from **Kenney** (<https://kenney.nl>), license
**Creative Commons Zero (CC0)** — "free to use in personal, educational and
commercial projects." No attribution required (credited here anyway).

| Theme / event       | Source pack       | Original file          |
|---------------------|-------------------|------------------------|
| modern / permission | Interface Sounds  | `confirmation_001.ogg` |
| modern / ask        | Interface Sounds  | `question_001.ogg`     |
| modern / done       | Interface Sounds  | `confirmation_003.ogg` |
| bells / permission  | Interface Sounds  | `glass_001.ogg`        |
| bells / ask         | Interface Sounds  | `glass_003.ogg`        |
| bells / done        | Interface Sounds  | `glass_006.ogg`        |
| arcade / permission | Digital Audio     | `pepSound1.ogg`        |
| arcade / ask        | Digital Audio     | `twoTone1.ogg`         |
| arcade / done       | Digital Audio     | `powerUp1.ogg`         |

Packs: Interface Sounds <https://kenney.nl/assets/interface-sounds>,
Digital Audio <https://kenney.nl/assets/digital-audio>.

## Pipeline

The Kenney packs ship OGG. We decode the chosen clips to mono 16-bit WAV (the
committed source-of-truth in `tools/sounds/`), then embed them:

1. **OGG → WAV** (one-off, needs an OGG decoder — not part of the repo build):
   decode each chosen clip to mono 16-bit WAV into `tools/sounds/`. The WAVs are
   committed so step 2 needs no network or decoder.
2. **WAV → C array**: `tools/build_sounds.sh` (→ `node tools/pcm_to_c.js`,
   pure Node, no ffmpeg) reads `tools/sounds.json`, resamples each WAV to
   16 kHz mono, trims to ≤1.2 s, applies ~6 ms fades, peak-normalizes to ~18000
   (matching the Retro synth amplitude), and writes
   `firmware/src/boards/waveshare_amoled_216_c6/audio_samples.h`.

### Changing a sound

Replace the WAV in `tools/sounds/` (or repoint `tools/sounds.json`), run
`tools/build_sounds.sh`, then rebuild:
`pio run -d firmware -e waveshare_amoled_216_c6`.
