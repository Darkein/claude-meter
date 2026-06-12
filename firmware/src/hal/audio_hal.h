#pragma once

// Optional notification-sound abstraction. Boards with audio hardware (e.g.
// the C6 AMOLED-2.16 with its ES8311 codec + speaker) play short chimes when
// the Claude session needs attention; boards without a speaker provide a
// no-op implementation so shared code can call into the HAL unconditionally.
//
// audio_hal_play() is fire-and-forget: it queues a chime and returns
// immediately. audio_hal_tick() (called once per loop) feeds the I2S DMA so
// nothing blocks the LVGL/BLE loop.

enum audio_sound_t {
    SND_ALERT = 0,   // Claude needs you: permission request or AskUserQuestion
    SND_DONE  = 1,   // Claude finished its turn (idle)
};

void audio_hal_init(void);
void audio_hal_tick(void);
void audio_hal_play(audio_sound_t sound);
