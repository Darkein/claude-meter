#pragma once
#include <stdint.h>
#include <lvgl.h>

// Mini animated creature for embedding in a screen (e.g. the idle screen or the
// header corner). Renders the named claudepix animation (e.g. "expression sleep")
// at ~px×px inside `parent`. Instance-based — each handle owns its own canvas and
// buffer, so several creatures can run at once. splash_mini_create() returns NULL
// if the animation isn't found / allocation fails. Drive each with
// splash_mini_tick().
typedef struct splash_mini splash_mini_t;

splash_mini_t *splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
lv_obj_t      *splash_mini_canvas(splash_mini_t *m);                          // position with lv_obj_align/set_pos
void           splash_mini_set_anim(splash_mini_t *m, const char *anim_name); // switch to a looping animation
void           splash_mini_play_once(splash_mini_t *m, const char *anim_name); // play once, then hold the last frame
void           splash_mini_freeze(splash_mini_t *m, const char *anim_name, int frame); // show one fixed frame, no motion
bool           splash_mini_busy(splash_mini_t *m);                            // true while a play_once is still running
void           splash_mini_tick(splash_mini_t *m);
