#include "../../hal/audio_hal.h"

// Optional: notification sounds. If your board has a speaker + codec/amp,
// implement these (see boards/waveshare_amoled_216_c6/audio.cpp for an ES8311
// + I2S example). Otherwise leave them as no-ops.

void audio_hal_init(void) {}
void audio_hal_tick(void) {}
void audio_hal_play(audio_sound_t) {}
void audio_hal_set_theme(uint8_t) {}
void audio_hal_set_volume(uint8_t) {}
uint8_t audio_hal_get_volume(void) { return 0; }
