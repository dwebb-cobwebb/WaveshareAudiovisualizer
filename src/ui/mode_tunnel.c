#include "ui/mode_tunnel.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"
#include "assets/tunnel_assets.h"

#include <math.h>
#include <string.h>

// ===========================================================================
// Tunnel view: audio-reactive infinite neon tunnel (classic demoscene LUT
// effect, fused straight into the QSPI blit — no framebuffer).
//
// Per pixel: polar LUT read (angle, depth) -> offset by fly/rotation phase ->
// texture fetch -> fogged palette lookup -> pushed to the panel. The palette
// is rebuilt every frame from the analyzer state, so the audio drives the
// visuals with zero per-pixel cost:
//   RMS level  -> fly-through speed + overall brightness
//   bass bands -> hue pulse
//   mid bands  -> rotation speed
//
// The LVGL object for this mode is just a black screen; frames are rendered
// directly to the panel from ui_update() while the mode is active. LVGL only
// repaints on mode switches (full_refresh), which cleanly overwrites us.
// ===========================================================================

#define FRAME_MS   33      // ~30 fps
#define FOG_LEVELS 8

// Texture is copied to RAM at init: per-pixel random access is much faster
// from SRAM than XIP flash.
static uint8_t s_tex[256 * 256];

static uint16_t s_fogpal[FOG_LEVELS][256];   // RGB565, ready-to-push

static lv_obj_t *s_root;
static uint32_t s_last_frame_ms;
static uint16_t s_fly8, s_rot8;    // 8.8 fixed-point phase accumulators
static uint8_t  s_hue;             // palette hue base (wraps)

// --- small HSV -> RGB565 helper (h 0..255, s/v 0..255) ---------------------
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

// Rebuild the fogged palette from the current audio state.
static void build_palette(float energy, float bass) {
    uint8_t hue = (uint8_t)(s_hue + (uint8_t)(bass * 24.0f));
    for (int t = 0; t < 256; t++) {
        float x = (float)t * (1.0f / 255.0f);
        float bright = 0.05f + (x * x) * (0.45f + 0.55f * energy);
        if (bright > 1.0f) bright = 1.0f;
        uint8_t sat = (uint8_t)(240 - (uint8_t)(120.0f * x * x * x));
        uint8_t val = (uint8_t)(bright * 255.0f);
        uint8_t h = (uint8_t)(hue + t / 12);
        uint16_t base = hsv565(h, sat, val);
        // 8 fog levels: index by unshifted LUT depth >> 5 (far = dark).
        uint8_t r5 = (base >> 11) & 0x1F, g6 = (base >> 5) & 0x3F, b5 = base & 0x1F;
        for (int f = 0; f < FOG_LEVELS; f++) {
            uint8_t k = (uint8_t)(255 - f * 34);         // 255..17
            s_fogpal[f][t] = (uint16_t)((((r5 * k) >> 8) << 11) |
                                        (((g6 * k) >> 8) << 5) |
                                        ((b5 * k) >> 8));
        }
    }
}

lv_obj_t *mode_tunnel_create(lv_obj_t *parent) {
    memcpy(s_tex, tunnel_tex, sizeof(s_tex));

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    return s_root;
}

void mode_tunnel_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    // Audio drive
    float energy = 0.5f * (vs->rms_l + vs->rms_r) * 3.0f;
    if (energy > 1.0f) energy = 1.0f;
    float bass = 0.0f, mid = 0.0f;
    for (int b = 0; b < 4; b++)  bass += vs->bands[b];
    bass *= 0.25f;
    for (int b = 10; b < 18; b++) mid += vs->bands[b];
    mid *= 0.125f;

    s_fly8 = (uint16_t)(s_fly8 + 260 + (uint16_t)(energy * 1400.0f));
    s_rot8 = (uint16_t)(s_rot8 + 40 + (uint16_t)(mid * 260.0f));
    s_hue  = (uint8_t)(s_hue + 1);

    uint8_t fly = (uint8_t)(s_fly8 >> 8);
    uint8_t rot = (uint8_t)(s_rot8 >> 8);

    build_palette(energy, bass);

    // Fused render + blit, portrait scan order (matches the panel GRAM).
    // Landscape mapping: lx = r, ly = 171 - c (FLIP_X=0, FLIP_Y=1), then
    // half-res LUT with 2x2 pixel doubling.
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        const uint8_t *col_lut = &tunnel_lut[(r >> 1) * TUNNEL_HALF_H * 2];
        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            uint32_t hy = (uint32_t)(171 - c) >> 1;
            const uint8_t *e = &col_lut[hy * 2];
            uint8_t d0 = e[1];
            uint8_t t = s_tex[(uint32_t)((uint8_t)(d0 + fly)) << 8 |
                              (uint8_t)(e[0] + rot)];
            uint16_t px = s_fogpal[d0 >> 5][t];
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
