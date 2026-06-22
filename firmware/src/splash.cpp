#include "splash.h"
#include "splash_animations.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// 20×20 pixel-art grid.
#define GRID         20
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

// ---- Mini creature: a small animated creature for embedding in a screen
//      (e.g. the idle "sleeping" indicator, or the state-driven header corner).
//      Instance-based — each handle owns its own canvas and buffer, so several
//      can run at once. ----
struct splash_mini {
    lv_obj_t  *canvas;
    uint16_t  *buf;
    int        cell;
    int        w;
    const splash_anim_def_t *anim;
    uint16_t   frame;
    uint32_t   started;
    bool       loop;   // true = wrap forever; false = one-shot or frozen
    bool       busy;   // (loop==false) true while a play_once is still advancing
};

static const splash_anim_def_t *find_anim(const char *anim_name) {
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        if (strcmp(splash_anims[i].name, anim_name) == 0) return &splash_anims[i];
    return NULL;
}

static void mini_render(splash_mini_t *m) {
    if (!m->buf || !m->anim) return;
    const uint8_t *cells = m->anim->frames[m->frame];
    const uint16_t *pal = m->anim->palette;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < m->cell; dy++) {
                uint16_t *dst = &m->buf[(gy * m->cell + dy) * m->w + gx * m->cell];
                for (int dx = 0; dx < m->cell; dx++) dst[dx] = color;
            }
        }
    }
    if (m->canvas) lv_obj_invalidate(m->canvas);
}

splash_mini_t *splash_mini_create(lv_obj_t *parent, const char *anim_name, int px) {
    const splash_anim_def_t *anim = find_anim(anim_name);
    if (!anim) return NULL;

    int cell = px / GRID;
    if (cell < 1) cell = 1;
    int w = GRID * cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    splash_mini_t *m = (splash_mini_t*)heap_caps_malloc(sizeof(splash_mini_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!m) return NULL;
    m->buf = (uint16_t*)heap_caps_malloc(w * w * 2, caps);
    if (!m->buf) { heap_caps_free(m); return NULL; }
    m->cell = cell;
    m->w = w;
    m->anim = anim;
    m->frame = 0;
    m->started = millis();
    m->loop = true;
    m->busy = false;
    m->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(m->canvas, m->buf, w, w, LV_COLOR_FORMAT_RGB565);
    mini_render(m);
    return m;
}

lv_obj_t *splash_mini_canvas(splash_mini_t *m) {
    return m ? m->canvas : NULL;
}

void splash_mini_set_anim(splash_mini_t *m, const char *anim_name) {
    if (!m) return;
    const splash_anim_def_t *anim = find_anim(anim_name);
    if (!anim) return;
    m->loop = true;   // (re)enter looping mode
    m->busy = false;
    if (anim == m->anim) return; // already on it — keep its current frame/loop
    m->anim = anim;
    m->frame = 0;
    m->started = millis();
    mini_render(m);
}

void splash_mini_play_once(splash_mini_t *m, const char *anim_name) {
    if (!m) return;
    const splash_anim_def_t *anim = find_anim(anim_name);
    if (!anim) return;
    m->anim = anim;
    m->frame = 0;
    m->started = millis();
    m->loop = false;
    m->busy = true;
    mini_render(m);
}

void splash_mini_freeze(splash_mini_t *m, const char *anim_name, int frame) {
    if (!m) return;
    const splash_anim_def_t *anim = find_anim(anim_name);
    if (!anim) return;
    if (frame < 0) frame = 0;
    if (frame >= anim->frame_count) frame = anim->frame_count - 1;
    m->anim = anim;
    m->frame = (uint16_t)frame;
    m->started = millis();
    m->loop = false;
    m->busy = false;
    mini_render(m);
}

bool splash_mini_busy(splash_mini_t *m) {
    return m && m->busy;
}

void splash_mini_tick(splash_mini_t *m) {
    if (!m || !m->buf || !m->anim || m->anim->frame_count == 0) return;
    if (!m->loop && !m->busy) return; // frozen on a fixed frame — nothing to do
    if (millis() - m->started < m->anim->holds[m->frame]) return;
    m->started = millis();
    if (m->frame + 1 >= m->anim->frame_count) {
        if (m->loop) { m->frame = 0; mini_render(m); }
        else { m->busy = false; } // one-shot finished — hold the last frame
    } else {
        m->frame++;
        mini_render(m);
    }
}
