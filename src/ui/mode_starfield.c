#include "ui/mode_starfield.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"

#include <string.h>

// ===========================================================================
// Starfield view: warp-speed starfield with motion trails.
//
// Stars live in 3D and are perspective-projected into a half-res brightness
// buffer (320x86, 2x2 pixel-doubled at blit time). Instead of clearing, the
// buffer fades each frame, so moving stars leave streaks — the faster the
// warp, the longer the trails. Audio drive:
//   RMS  -> warp speed (longer, brighter streaks when loud)
//   bass -> subtle warm flash on the star colour
// ===========================================================================

#define FRAME_MS   33
#define NSTARS     220
#define HW         320
#define HH         86
#define Z_MAX      4095
#define Z_MIN      40
#define FADE_K     225      // trail persistence per frame (of 256)

typedef struct { int16_t x, y; int16_t z; } star_t;

static uint8_t  s_buf[HW * HH];      // column-major: [hx*HH + hy]
static star_t   s_stars[NSTARS];
static uint16_t s_pal[256];
static lv_obj_t *s_root;
static uint32_t s_last_frame_ms;
static uint32_t s_rng = 0x2A5F19C3u;
static float    s_env;   // smoothed energy envelope (0..1)

static inline uint32_t xrand(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void respawn(star_t *s, bool anywhere) {
    s->x = (int16_t)((int32_t)(xrand() & 0x0FFF) - 2048);
    s->y = (int16_t)((int32_t)(xrand() & 0x0FFF) - 2048);
    s->z = anywhere ? (int16_t)(Z_MIN + (xrand() % (Z_MAX - Z_MIN)))
                    : (int16_t)Z_MAX;
}

lv_obj_t *mode_starfield_create(lv_obj_t *parent) {
    memset(s_buf, 0, sizeof(s_buf));
    for (int i = 0; i < NSTARS; i++) respawn(&s_stars[i], true);

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    return s_root;
}

void mode_starfield_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    float energy = 0.5f * (vs->rms_l + vs->rms_r) * 3.0f;
    if (energy > 1.0f) energy = 1.0f;
    float bass = 0.0f;
    for (int b = 0; b < 4; b++) bass += vs->bands[b];
    bass *= 0.25f;
    // Fast attack, slow release envelope drives how many stars fly.
    s_env += (energy - s_env) * (energy > s_env ? 0.5f : 0.06f);

    // Fade trails.
    for (uint32_t i = 0; i < sizeof(s_buf); i++) {
        s_buf[i] = (uint8_t)((s_buf[i] * FADE_K) >> 8);
    }

    // Advance + plot stars. The active star count scales with the envelope:
    // empty space in silence, a full warp field when the track kicks off.
    // Speed follows env^2 so the field mostly drifts and only hits full warp
    // on genuinely loud passages.
    int speed = 3 + (int)(s_env * s_env * 40.0f);
    int active = (int)((float)NSTARS * s_env * (2.0f - s_env));  // fast ramp-in
    if (active > NSTARS) active = NSTARS;
    for (int i = 0; i < active; i++) {
        star_t *s = &s_stars[i];
        s->z = (int16_t)(s->z - speed);
        if (s->z < Z_MIN) { respawn(s, false); continue; }
        int32_t px = (HW / 2) + ((int32_t)s->x * 40) / (s->z + 1);
        int32_t py = (HH / 2) + ((int32_t)s->y * 40) / (s->z + 1);
        if (px < 0 || px >= HW || py < 0 || py >= HH) { respawn(s, false); continue; }
        uint8_t b = (uint8_t)(255 - (s->z >> 4));
        uint8_t *cell = &s_buf[px * HH + py];
        if (b > *cell) *cell = b;
    }

    // Palette: blue-white stars; bass adds a subtle warm flash.
    int warm = (int)(bass * 70.0f);
    for (int v = 0; v < 256; v++) {
        int r = (v * (225 + warm)) >> 8;
        int g = (v * 235) >> 8;
        int b = v + (v >> 3);
        if (r > 255) r = 255;
        if (b > 255) b = 255;
        s_pal[v] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    // Fused blit (portrait scan; lx = r, ly = 171 - c; 2x2 doubling).
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        const uint8_t *col = &s_buf[(r >> 1) * HH];
        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            uint16_t px = s_pal[col[(uint32_t)(171 - c) >> 1]];
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
