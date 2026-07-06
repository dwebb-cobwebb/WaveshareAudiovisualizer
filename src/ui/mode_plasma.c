#include "ui/mode_plasma.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"
#include "assets/plasma_assets.h"

#include <math.h>

// ===========================================================================
// Plasma view: classic sum-of-sines plasma, audio-reactive, rendered with the
// same fused blit as the tunnel (no framebuffer).
//
// v = f(x) + g(y) + sin(radial + t4); f and g are 1-D arrays rebuilt each
// frame, the radial distance is a half-res flash LUT. Audio drive:
//   RMS  -> palette brightness
//   bass -> radial pulse speed (rings breathe outward on hits) + hue kick
//   mids -> lateral drift speed
// ===========================================================================

#define FRAME_MS 33

static uint8_t  s_sin[256];
static uint16_t s_pal[256];
static uint16_t s_fx[PLASMA_HALF_W];   // sum of two sines, 0..510
static uint8_t  s_gy[PLASMA_HALF_H];

static lv_obj_t *s_root;
static uint32_t s_last_frame_ms;
static uint16_t s_t1, s_t2, s_t3, s_t4;   // 8.8 phase accumulators
static uint8_t  s_hue;

static uint16_t hsv565(uint8_t h, uint8_t sat, uint8_t val) {
    uint8_t region = h / 43;
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((val * (255 - sat)) >> 8);
    uint8_t q = (uint8_t)((val * (255 - ((sat * rem) >> 8))) >> 8);
    uint8_t t = (uint8_t)((val * (255 - ((sat * (255 - rem)) >> 8))) >> 8);
    uint8_t r, g, b;
    switch (region) {
        case 0:  r = val; g = t;   b = p;   break;
        case 1:  r = q;   g = val; b = p;   break;
        case 2:  r = p;   g = val; b = t;   break;
        case 3:  r = p;   g = q;   b = val; break;
        case 4:  r = t;   g = p;   b = val; break;
        default: r = val; g = p;   b = q;   break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

lv_obj_t *mode_plasma_create(lv_obj_t *parent) {
    for (int i = 0; i < 256; i++) {
        s_sin[i] = (uint8_t)(128.0f + 127.0f * sinf((float)i * (float)M_PI / 128.0f));
    }
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    return s_root;
}

void mode_plasma_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    float energy = 0.5f * (vs->rms_l + vs->rms_r) * 3.0f;
    if (energy > 1.0f) energy = 1.0f;
    float bass = 0.0f, mid = 0.0f;
    for (int b = 0; b < 4; b++)  bass += vs->bands[b];
    bass *= 0.25f;
    for (int b = 10; b < 18; b++) mid += vs->bands[b];
    mid *= 0.125f;

    s_t1 = (uint16_t)(s_t1 + 150 + (uint16_t)(mid * 700.0f));
    s_t2 = (uint16_t)(s_t2 + 210 + (uint16_t)(mid * 500.0f));
    s_t3 = (uint16_t)(s_t3 - 110);
    s_t4 = (uint16_t)(s_t4 + 180 + (uint16_t)(bass * 1600.0f));
    s_hue = (uint8_t)(s_hue + 1 + (uint8_t)(bass * 3.0f));

    uint8_t t1 = (uint8_t)(s_t1 >> 8), t2 = (uint8_t)(s_t2 >> 8);
    uint8_t t3 = (uint8_t)(s_t3 >> 8), t4 = (uint8_t)(s_t4 >> 8);

    // Palette: rotating hue over the plasma value, brightness follows RMS.
    float amp = 0.45f + 0.55f * energy;
    for (int v = 0; v < 256; v++) {
        uint8_t val = (uint8_t)((40.0f + 215.0f * (float)s_sin[(v * 2) & 255]
                                 * (1.0f / 255.0f)) * amp);
        s_pal[v] = hsv565((uint8_t)(s_hue + v / 2), 235, val);
    }

    // Per-frame 1-D contributions.
    for (int x = 0; x < PLASMA_HALF_W; x++) {
        s_fx[x] = (uint16_t)(s_sin[(uint8_t)(x * 2 + t1)] + s_sin[(uint8_t)(x + t3)]);
    }
    for (int y = 0; y < PLASMA_HALF_H; y++) {
        s_gy[y] = s_sin[(uint8_t)(y * 3 + t2)];
    }

    // Fused render + blit (portrait scan; lx = r, ly = 171 - c; 2x2 doubling).
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        const uint8_t *dist_col = &plasma_dist[(r >> 1) * PLASMA_HALF_H];
        uint16_t fx = s_fx[r >> 1];
        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            uint32_t hy = (uint32_t)(171 - c) >> 1;
            uint32_t v = (uint32_t)(fx + s_gy[hy] +
                                    s_sin[(uint8_t)(dist_col[hy] + t4)]) >> 2;
            uint16_t px = s_pal[v & 0xFF];
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
