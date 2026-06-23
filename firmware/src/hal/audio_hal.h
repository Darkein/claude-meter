#pragma once
#include <stdint.h>

// Optional notification-sound abstraction. Boards with audio hardware (e.g.
// the C6 AMOLED-2.16 with its ES8311 codec + speaker) play short chimes when
// the Claude session needs attention; boards without a speaker provide a
// no-op implementation so shared code can call into the HAL unconditionally.
//
// audio_hal_play() is fire-and-forget: it queues a chime and returns
// immediately. audio_hal_tick() (called once per loop) feeds the I2S DMA so
// nothing blocks the LVGL/BLE loop.

enum audio_sound_t {
    SND_PERMISSION = 0,  // cs=2 CLAUDE_WAITING — a tool-permission prompt is pending
    SND_ASK        = 1,  // cs=4 CLAUDE_QUESTION — plan approval or AskUserQuestion
    SND_DONE       = 2,  // WORKING -> IDLE — Claude finished its turn
};

void audio_hal_init(void);
void audio_hal_tick(void);
void audio_hal_play(audio_sound_t sound);

// Select the active sound theme by index (0 = Retro synth chimes; higher
// indices map to sampled themes on boards that bundle them). Boards without
// audio no-op. Persistence lives in soundtheme.{h,cpp}, not here.
void audio_hal_set_theme(uint8_t idx);

// Volume 0..255 (0 = off/mute, 255 = loudest), applied continuously to the
// codec amplitude. Boards without audio no-op on set and return 0 on get.
// Persistence lives in volume.{h,cpp}, not here.
void    audio_hal_set_volume(uint8_t val);
uint8_t audio_hal_get_volume(void);
