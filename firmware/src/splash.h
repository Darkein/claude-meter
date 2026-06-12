#pragma once
#include <stdint.h>
#include <lvgl.h>

// Mini animated creature for embedding in a screen (e.g. the idle screen).
// Renders the named claudepix animation (e.g. "expression sleep") at ~px×px
// inside `parent`; returns the canvas object (position it with lv_obj_align) or
// NULL if the animation isn't found / allocation fails. Drive it with
// splash_mini_tick(). One mini creature at a time.
lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
void splash_mini_tick(void);
