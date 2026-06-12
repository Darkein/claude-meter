#include "../../hal/audio_hal.h"

// No speaker on the S3 AMOLED-2.16 — notification sounds are a no-op.

void audio_hal_init(void) {}
void audio_hal_tick(void) {}
void audio_hal_play(audio_sound_t) {}
