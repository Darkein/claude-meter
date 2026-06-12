#include "../../hal/audio_hal.h"

// The AMOLED-1.8 has an audio amp (EXIO2) but no speaker is wired in the
// reference kit and no codec driver is implemented — notification sounds are
// a no-op here.

void audio_hal_init(void) {}
void audio_hal_tick(void) {}
void audio_hal_play(audio_sound_t) {}
