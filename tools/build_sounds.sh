#!/usr/bin/env bash
# Regenerate firmware/src/boards/waveshare_amoled_216_c6/audio_samples.h from
# the WAV clips in tools/sounds/ (mapped by tools/sounds.json).
#
# Pure Node — no ffmpeg. Source WAVs are decoded, resampled to 16 kHz mono,
# trimmed, faded, peak-normalized and embedded as C arrays. See SOUNDS.md for
# the sound sources and licenses.
set -euo pipefail
cd "$(dirname "$0")/.."
node tools/pcm_to_c.js
echo "Done. Rebuild: pio run -d firmware -e waveshare_amoled_216_c6"
