#pragma once
#include <stdint.h>

// User-selected notification sound theme, persisted to NVS. Mirrors
// sleeptimeout/volume. Index 0 is "Retro" (the original synth chimes); higher
// indices are sampled themes. The chosen theme routes through
// audio_hal_set_theme(). The Settings screen renders this as a dropdown, so
// soundtheme_label() feeds its option list.
void        soundtheme_init(void);          // load saved idx from NVS and apply
void        soundtheme_set(uint8_t idx);    // select theme, persist, apply, audition
uint8_t     soundtheme_get(void);           // current theme index
uint8_t     soundtheme_count(void);         // number of themes
const char* soundtheme_label(uint8_t idx);  // human label, e.g. "Retro" / "Modern"
