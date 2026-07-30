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
// if it isn't 1:1. Those side letterbox panels aren't wasted: the left one
// holds a phase/correlation meter (reusing VisualizerState's existing
// correlation field) and the right one holds a pair of mid/side level
// meters (derived from the same gonio_l/gonio_r samples already being
// plotted — no new analyzer work or VisualizerState growth needed).
// ===========================================================================

#define FRAME_MS        33
#define GH              (AV_DISP_H / 2)         // 86 — half-res square side
#define SQ_NATIVE       AV_DISP_NATIVE_W         // 172 — native square size (height-limited)
#define SQ_R0           ((AV_DISP_NATIVE_H - SQ_NATIVE) / 2)  // letterbox offset
#define MARGIN          4
#define CENTER          (GH / 2)
#define RADIUS          (GH / 2 - MARGIN)
#define DENSITY_STEP    55

// Phase/correlation meter (left letterbox panel, r in [0, SQ_R0)).
#define PHASE_MARGIN    24
#define PHASE_X0        PHASE_MARGIN
#define PHASE_X1        (SQ_R0 - PHASE_MARGIN)
#define PHASE_Y         (AV_DISP_H / 2)
#define PHASE_TICK_HALF 6
#define PHASE_IND_HALF  6
#define PHASE_IND_HW    2

// Mid/side level meters (right letterbox panel, r in [SQ_R0+SQ_NATIVE, 640)).
// Top margin is minimal — the M/S labels sit inside the bars themselves
// (near the top) rather than in a reserved strip above them, so the meters
// get the full available height.
#define MET_TOP     6
#define MET_BOT     152
#define MET_H       (MET_BOT - MET_TOP)
#define MET_W       44
#define MET_GAP     24
#define MET_R0      (SQ_R0 + SQ_NATIVE)
#define MET_AREA_W  (AV_DISP_NATIVE_H - MET_R0)
#define MET_M_X0    (MET_R0 + (MET_AREA_W - (2 * MET_W + MET_GAP)) / 2)
#define MET_M_X1    (MET_M_X0 + MET_W)
#define MET_S_X0    (MET_M_X1 + MET_GAP)
#define MET_S_X1    (MET_S_X0 + MET_W)

// Tiny 5x5 bitmap labels ("M"/"S" inside the level meters, "-"/"+" at the
// phase meter's ends) — this view bypasses LVGL's own renderer like
// mode_spectrum.c, so labels are baked into the same pixel stream rather
// than drawn as text widgets. Kept as an independent, minimal font (just
// the 4 glyphs needed) rather than sharing mode_spectrum.c's, matching this
// file's existing preference for standalone state. 5 columns (rather than
// mode_spectrum.c's 3) so "M" doesn't collapse into looking like an "H".
#define GLYPH_W     5
#define GLYPH_H     5
#define LBL_SCALE   2
#define LBL_GW      (GLYPH_W * LBL_SCALE)
#define LBL_GH      (GLYPH_H * LBL_SCALE)

#define LBL_M_X0    (MET_M_X0 + (MET_W - LBL_GW) / 2)
#define LBL_S_X0    (MET_S_X0 + (MET_W - LBL_GW) / 2)
#define LBL_MS_Y0   (MET_TOP + 3)

#define LBL_MINUS_X0 ((PHASE_X0 - LBL_GW) / 2)
#define LBL_PLUS_X0  (PHASE_X1 + (SQ_R0 - PHASE_X1 - LBL_GW) / 2)
#define LBL_PM_Y0    (PHASE_Y - LBL_GH / 2)

static const uint8_t FONT_M[GLYPH_H]     = { 0x11, 0x1B, 0x15, 0x11, 0x11 };
static const uint8_t FONT_S[GLYPH_H]     = { 0x0F, 0x10, 0x0E, 0x01, 0x1E };
static const uint8_t FONT_PLUS[GLYPH_H]  = { 0x04, 0x04, 0x1F, 0x04, 0x04 };
static const uint8_t FONT_MINUS[GLYPH_H] = { 0x00, 0x00, 0x1F, 0x00, 0x00 };

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

// Phase meter colours.
static uint16_t s_phase_track_col;
static uint16_t s_phase_tick_col;
static uint16_t s_phase_ind_pos_col;   // correlation >= 0 (in phase)
static uint16_t s_phase_ind_neg_col;   // correlation <  0 (out of phase)

// Mid/side meter colours + smoothed levels (attack/decay ballistics, same
// idea as the band ballistics in analyzer.c — just computed here on the UI
// core from data already being read for the scatter plot).
static uint16_t s_met_track_col;
static uint16_t s_met_m_col;
static uint16_t s_met_s_col;
static float    s_m_smooth = 0.0f;
static float    s_s_smooth = 0.0f;

static uint16_t s_label_col;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Whether native pixel (r, ly) falls on a lit cell of the glyph anchored at
// (x0, y0). Positions/content here are all fixed, so this is checked
// straightforwardly per-pixel rather than needing mode_spectrum.c's dynamic
// string handling.
static bool glyph_lit(int r, int ly, int x0, int y0, const uint8_t *rows) {
    int col = (r - x0) / LBL_SCALE;
    int row = (ly - y0) / LBL_SCALE;
    if (col < 0 || col >= GLYPH_W || row < 0 || row >= GLYPH_H) return false;
    return (rows[row] >> (GLYPH_W - 1 - col)) & 1;
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

    s_phase_track_col   = rgb565(40, 40, 44);
    s_phase_tick_col    = rgb565(75, 75, 82);
    s_phase_ind_pos_col = rgb565(120, 240, 150);
    s_phase_ind_neg_col = rgb565(230, 90, 70);

    s_met_track_col = rgb565(40, 40, 44);
    s_met_m_col     = rgb565(215, 205, 165);   // mid: warm white
    s_met_s_col     = rgb565(170, 130, 235);   // side: violet

    s_label_col = rgb565(150, 150, 158);

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
    // reads as a continuous path rather than a dotted scatter. Also
    // accumulates mid/side energy for the level meters — same mid/side
    // values already being computed here for the plot, just squared and
    // summed, so the meters cost one extra multiply-add per point.
    int prev_gx = -1, prev_gy = -1;
    float sum_m2 = 0.0f, sum_s2 = 0.0f;
    for (int i = 0; i < AV_GONIO_POINTS; i++) {
        float l = (float)vs->gonio_l[i] / 127.0f;
        float r = (float)vs->gonio_r[i] / 127.0f;
        float mid  = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;
        sum_m2 += mid * mid;
        sum_s2 += side * side;
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

    // Mid/side RMS-ish levels, gained up a bit (RMS of program material sits
    // well under 1.0) and given simple attack/decay ballistics so the bars
    // don't jitter — same idea as the band ballistics in analyzer.c.
    float m_target = sqrtf(sum_m2 / (float)AV_GONIO_POINTS) * 2.2f;
    float s_target = sqrtf(sum_s2 / (float)AV_GONIO_POINTS) * 2.2f;
    if (m_target > 1.0f) m_target = 1.0f;
    if (s_target > 1.0f) s_target = 1.0f;
    s_m_smooth += (m_target - s_m_smooth) * ((m_target > s_m_smooth) ? 0.5f : 0.08f);
    s_s_smooth += (s_target - s_s_smooth) * ((s_target > s_s_smooth) ? 0.5f : 0.08f);

    // Phase meter position (indicator column + which side of centre) and
    // meter bar fill heights are frame-constant — compute once, not per row.
    int corr_x = PHASE_X0 + (int)((vs->correlation * 0.5f + 0.5f) * (float)(PHASE_X1 - PHASE_X0) + 0.5f);
    corr_x = clampi(corr_x, PHASE_X0, PHASE_X1);
    int center_x = (PHASE_X0 + PHASE_X1) / 2;
    uint16_t ind_col = (vs->correlation >= 0.0f) ? s_phase_ind_pos_col : s_phase_ind_neg_col;

    int m_fill_top = MET_BOT - (int)(s_m_smooth * (float)MET_H + 0.5f);
    int s_fill_top = MET_BOT - (int)(s_s_smooth * (float)MET_H + 0.5f);

    // Fused blit (portrait scan; lx = r, ly = 171 - c; bilinear 2x2 upscale)
    // — same pattern as mode_oscope.c, but letterboxed to a centered square
    // since the plot must stay 1:1. The side letterbox columns render the
    // phase meter (left) and mid/side level meters (right) instead of black.
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        bool in_sq = (r >= SQ_R0 && r < SQ_R0 + SQ_NATIVE);
        int hx_m = 0, hx_o = 0;
        if (in_sq) {
            int rr = (int)(r - SQ_R0);
            hx_m = rr >> 1;
            hx_o = clampi((rr & 1) ? hx_m + 1 : hx_m - 1, 0, GH - 1);
        }

        bool in_phase_x   = (int)r >= PHASE_X0 && (int)r <= PHASE_X1;
        bool is_ctr_tick   = (int)r == center_x;
        bool is_indicator  = abs((int)r - corr_x) <= PHASE_IND_HW;
        bool in_m_x        = (int)r >= MET_M_X0 && (int)r < MET_M_X1;
        bool in_s_x        = (int)r >= MET_S_X0 && (int)r < MET_S_X1;

        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            int ly = 171 - c;
            uint16_t px;
            if (in_sq) {
                uint32_t d = (uint32_t)(171 - c);
                int hy_m = (int)(d >> 1);
                int hy_o = clampi((d & 1) ? hy_m + 1 : hy_m - 1, 0, GH - 1);
                int b = bilerp4(hx_m, hx_o, hy_m, hy_o);
                px = (b == 0) ? s_grid_col[grid_at(hx_m, hy_m)] : s_pal[b];
            } else if ((int)r < SQ_R0) {
                if (glyph_lit((int)r, ly, LBL_MINUS_X0, LBL_PM_Y0, FONT_MINUS) ||
                    glyph_lit((int)r, ly, LBL_PLUS_X0, LBL_PM_Y0, FONT_PLUS)) {
                    px = s_label_col;
                } else if (is_indicator && abs(ly - PHASE_Y) <= PHASE_IND_HALF) {
                    px = ind_col;
                } else if (is_ctr_tick && abs(ly - PHASE_Y) <= PHASE_TICK_HALF) {
                    px = s_phase_tick_col;
                } else if (in_phase_x && abs(ly - PHASE_Y) <= 1) {
                    px = s_phase_track_col;
                } else {
                    px = 0;
                }
            } else {
                if (glyph_lit((int)r, ly, LBL_M_X0, LBL_MS_Y0, FONT_M) ||
                    glyph_lit((int)r, ly, LBL_S_X0, LBL_MS_Y0, FONT_S)) {
                    px = s_label_col;
                } else if (in_m_x && ly >= MET_TOP && ly <= MET_BOT) {
                    px = (ly >= m_fill_top) ? s_met_m_col : s_met_track_col;
                } else if (in_s_x && ly >= MET_TOP && ly <= MET_BOT) {
                    px = (ly >= s_fill_top) ? s_met_s_col : s_met_track_col;
                } else {
                    px = 0;
                }
            }
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
