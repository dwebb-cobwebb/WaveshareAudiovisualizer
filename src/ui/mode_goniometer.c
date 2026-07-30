#include "ui/mode_goniometer.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

// ===========================================================================
// Goniometer view: stereo M/S (mid/side) scatter plot, aka vectorscope.
//
// Each frame's raw (untriggered) sample pairs are rotated into mid/side
// space — mid = (L+R)/2 drawn vertically, side = (L-R)/2 drawn horizontally —
// so mono content draws a vertical line and fully out-of-phase content draws
// a horizontal line, matching a classic hardware goniometer. Consecutive
// points are connected (not just dotted) and each touched cell's brightness
// accumulates and saturates rather than snapping straight to full, so
// frequently-visited regions glow brighter — density-weighted, like real CRT
// phosphor — and fade via the same tap-to-cycle trail levels as the
// oscilloscope view.
//
// The plot is a square (bounded by the panel's landscape height) centered in
// the middle of the screen, letterboxed left/right — a goniometer distorts
// if it isn't 1:1.
// ===========================================================================

#define FRAME_MS        33
#define GH              (AV_DISP_H / 2)         // 86 — half-res square side
#define SQ_NATIVE       AV_DISP_NATIVE_W         // 172 — native square size (height-limited)
#define SQ_R0           ((AV_DISP_NATIVE_H - SQ_NATIVE) / 2)  // letterbox offset
#define MARGIN          4
#define CENTER          (GH / 2)
#define RADIUS          (GH / 2 - MARGIN)
#define DENSITY_STEP    55

enum { GRID_NONE = 0, GRID_DIM, GRID_CENTER };

// Trail persistence per frame (of 256), none to loads. Cycled by tap_cb.
// Same levels as mode_oscope.c, kept as an independent copy since the two
// views intentionally don't share state or buffers.
static const uint8_t s_trail_levels[5] = { 0, 130, 195, 225, 248 };
static int s_trail_level = 2;

static uint8_t   s_buf[GH * GH];     // column-major density/brightness
static uint16_t  s_pal[256];         // brightness -> RGB565 ramp (green phosphor)
static uint16_t  s_grid_col[3];      // GRID_NONE/DIM/CENTER -> RGB565
static lv_obj_t *s_root;
static uint32_t  s_last_frame_ms;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Crosshair + diagonal L/R axes + an outer full-scale ring, all computed on
// the fly (no static mask) — same reasoning as mode_oscope.c's grid_at.
static uint8_t grid_at(int gx, int gy) {
    if (gx == CENTER || gy == CENTER) return GRID_CENTER;
    int dx = gx - CENTER, dy = gy - CENTER;
    if (dx == dy || dx == -dy) return GRID_DIM;
    float dist = sqrtf((float)(dx * dx + dy * dy));
    if (fabsf(dist - (float)RADIUS) < 0.75f) return GRID_DIM;
    return GRID_NONE;
}

// 2x2 bilinear tap, same fixed 3:1-per-axis weighting as mode_oscope.c.
static inline int bilerp4(int hx_m, int hx_o, int hy_m, int hy_o) {
    int sum = s_buf[hx_m * GH + hy_m] * 9
            + s_buf[hx_m * GH + hy_o] * 3
            + s_buf[hx_o * GH + hy_m] * 3
            + s_buf[hx_o * GH + hy_o] * 1;
    return (sum + 8) >> 4;
}

static inline void bump(int x, int y) {
    int idx = x * GH + y;
    int v = s_buf[idx] + DENSITY_STEP;
    s_buf[idx] = (v > 255) ? 255 : (uint8_t)v;
}

static void tap_cb(lv_event_t *e) {
    (void)e;
    s_trail_level = (s_trail_level + 1) % 5;
}

lv_obj_t *mode_goniometer_create(lv_obj_t *parent) {
    memset(s_buf, 0, sizeof(s_buf));

    s_grid_col[GRID_NONE]   = rgb565(0, 0, 0);
    s_grid_col[GRID_DIM]    = rgb565(40, 46, 40);
    s_grid_col[GRID_CENTER] = rgb565(75, 82, 75);

    // Gamma-corrected green phosphor ramp — see mode_oscope.c for why a
    // linear v/255 mix reads as dim in the mid tones.
    for (int v = 0; v < 256; v++) {
        float g = powf((float)v / 255.0f, 0.6f);
        s_pal[v] = rgb565((uint8_t)(80.0f * g), (uint8_t)(255.0f * g), (uint8_t)(120.0f * g));
    }

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, tap_cb, LV_EVENT_CLICKED, NULL);
    return s_root;
}

void mode_goniometer_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    // Fade the trail.
    uint8_t fade = s_trail_levels[s_trail_level];
    for (uint32_t i = 0; i < sizeof(s_buf); i++) {
        s_buf[i] = (uint8_t)((s_buf[i] * fade) >> 8);
    }

    // Plot this window's points, connecting consecutive samples so the trace
    // reads as a continuous path rather than a dotted scatter.
    int prev_gx = -1, prev_gy = -1;
    for (int i = 0; i < AV_GONIO_POINTS; i++) {
        float l = (float)vs->gonio_l[i] / 127.0f;
        float r = (float)vs->gonio_r[i] / 127.0f;
        float mid  = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;
        int gx = clampi(CENTER + (int)(side * (float)RADIUS), 0, GH - 1);
        int gy = clampi(CENTER - (int)(mid  * (float)RADIUS), 0, GH - 1);

        if (prev_gx < 0) {
            bump(gx, gy);
        } else {
            int steps = abs(gx - prev_gx);
            int dy = abs(gy - prev_gy);
            if (dy > steps) steps = dy;
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; s++) {
                bump(prev_gx + (gx - prev_gx) * s / steps,
                     prev_gy + (gy - prev_gy) * s / steps);
            }
        }
        prev_gx = gx;
        prev_gy = gy;
    }

    // Fused blit (portrait scan; lx = r, ly = 171 - c; bilinear 2x2 upscale)
    // — same pattern as mode_oscope.c, but letterboxed to a centered square
    // since the plot must stay 1:1.
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        bool in_sq = (r >= SQ_R0 && r < SQ_R0 + SQ_NATIVE);
        int hx_m = 0, hx_o = 0;
        if (in_sq) {
            int rr = (int)(r - SQ_R0);
            hx_m = rr >> 1;
            hx_o = clampi((rr & 1) ? hx_m + 1 : hx_m - 1, 0, GH - 1);
        }
        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            uint16_t px;
            if (!in_sq) {
                px = 0;
            } else {
                uint32_t d = (uint32_t)(171 - c);
                int hy_m = (int)(d >> 1);
                int hy_o = clampi((d & 1) ? hy_m + 1 : hy_m - 1, 0, GH - 1);
                int b = bilerp4(hx_m, hx_o, hy_m, hy_o);
                px = (b == 0) ? s_grid_col[grid_at(hx_m, hy_m)] : s_pal[b];
            }
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
