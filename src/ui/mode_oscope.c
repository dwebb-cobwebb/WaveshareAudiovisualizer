#include "ui/mode_oscope.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"

#include <string.h>
#include <math.h>

// ===========================================================================
// Oscilloscope view: dual-trace stereo waveform over a graticule, with a
// phosphor-style fading trail.
//
// Each column holds the min/max sample envelope for that slice of the most
// recent analysis window (see analyzer_process's trigger + column mapping in
// dsp/analyzer.c), so fast transients still show their full excursion even
// though the window is decimated to 320 columns. A rising zero-crossing
// trigger keeps periodic/tonal content holding still instead of sliding
// frame to frame.
//
// L and R are each drawn into their own fading brightness buffer (so old
// trace pixels glow and decay independently, like CRT phosphor) and additively
// blended at blit time — pixels where both channels light up read brighter/
// whiter, which makes in-phase content stand out.
//
// Tap anywhere on the view to cycle the trail length through 5 levels, none
// to loads (see s_trail_levels).
//
// Two smoothing passes hide the underlying 320x86 half-res buffer: plot_span
// gives each column's min/max edge fractional (coverage-based) brightness
// instead of an all-or-nothing pixel, and the 2x2 upscale at blit time is
// bilinear rather than nearest-neighbour, so what would otherwise be flat
// doubled blocks blend into their neighbours.
// ===========================================================================

#define FRAME_MS    33
#define HW          AV_SCOPE_COLS      // 320 — landscape width, half-res
#define HH          (AV_DISP_H / 2)    // 86  — landscape height, half-res
#define MARGIN      4
#define BASELINE    (HH / 2)
#define AMP_SCALE   ((float)(HH / 2 - MARGIN) / 127.0f)

enum { GRID_NONE = 0, GRID_DIM, GRID_CENTER };

// Trail persistence per frame (of 256), none to loads. Cycled by tap_cb.
static const uint8_t s_trail_levels[5] = { 0, 130, 195, 225, 248 };
static int s_trail_level = 2;   // start at the previously-tuned "medium" trail

static uint8_t   s_buf_l[HW * HH];   // column-major fading brightness, L
static uint8_t   s_buf_r[HW * HH];   // column-major fading brightness, R
static uint16_t  s_pal_l[256];       // brightness -> RGB565 ramp, ch1 (amber)
static uint16_t  s_pal_r[256];       // brightness -> RGB565 ramp, ch2 (cyan)
static uint16_t  s_grid_col[3];      // GRID_NONE/DIM/CENTER -> RGB565
static lv_obj_t *s_root;
static uint32_t  s_last_frame_ms;

// Graticule computed on the fly at blit time rather than stored in a static
// mask — the vertical-division check only depends on the column (hoisted out
// of the inner loop), and the horizontal checks are a few int compares, so
// this costs nothing but saves a third HW*HH byte buffer.
static bool is_vdiv(int x) {
    for (int i = 1; i < 8; i++) if (x == (HW * i) / 8) return true;
    return false;
}

static uint8_t grid_at(bool vdiv, int y) {
    if (y == BASELINE) return GRID_CENTER;
    int top = BASELINE - (HH / 2 - MARGIN), bot = BASELINE + (HH / 2 - MARGIN);
    if (y == top || y == bot) return GRID_DIM;
    return vdiv ? GRID_DIM : GRID_NONE;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Additive blend of two RGB565 colours, clamped per-channel.
static uint16_t blend565(uint16_t a, uint16_t b) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + br; if (r > 0x1F) r = 0x1F;
    int g = ag + bg; if (g > 0x3F) g = 0x3F;
    int bl = ab + bb; if (bl > 0x1F) bl = 0x1F;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Plots a column's [ytop, ybot] envelope (fractional half-res rows) at full
// brightness, except the two boundary rows which get partial brightness
// proportional to how much of that row the span actually covers — cheap
// vertical anti-aliasing (320 columns/frame, so the float math costs nothing
// next to the 110k-pixel blit).
static void plot_span(uint8_t *buf, int x, float ytop, float ybot) {
    if (ytop > ybot) { float t = ytop; ytop = ybot; ybot = t; }
    if (ybot < 0.0f || ytop > (float)(HH - 1)) return;
    if (ytop < 0.0f) ytop = 0.0f;
    if (ybot > (float)(HH - 1)) ybot = (float)(HH - 1);

    int row_top = (int)floorf(ytop);
    int row_bot = (int)floorf(ybot);
    uint8_t *base = &buf[x * HH];

    if (row_top == row_bot) {
        base[row_top] = 255;
        return;
    }
    uint8_t top_b = (uint8_t)((1.0f - (ytop - (float)row_top)) * 255.0f);
    if (top_b > base[row_top]) base[row_top] = top_b;
    for (int y = row_top + 1; y < row_bot; y++) base[y] = 255;
    uint8_t bot_b = (uint8_t)((ybot - (float)row_bot) * 255.0f);
    if (bot_b > base[row_bot]) base[row_bot] = bot_b;
}

// 2x2 bilinear tap: (main,main) gets weight 9, the two (main,other) taps get
// weight 3 each, (other,other) gets weight 1 — the standard fixed 3:1 per-axis
// bilinear weighting for an exact 2x upscale, combined here as a 16ths sum so
// it's pure integer math.
static inline int bilerp4(const uint8_t *buf, int hx_m, int hx_o, int hy_m, int hy_o) {
    int sum = buf[hx_m * HH + hy_m] * 9
            + buf[hx_m * HH + hy_o] * 3
            + buf[hx_o * HH + hy_m] * 3
            + buf[hx_o * HH + hy_o] * 1;
    return (sum + 8) >> 4;
}

static void tap_cb(lv_event_t *e) {
    (void)e;
    s_trail_level = (s_trail_level + 1) % 5;
}

lv_obj_t *mode_oscope_create(lv_obj_t *parent) {
    memset(s_buf_l, 0, sizeof(s_buf_l));
    memset(s_buf_r, 0, sizeof(s_buf_r));

    s_grid_col[GRID_NONE]   = rgb565(0, 0, 0);
    s_grid_col[GRID_DIM]    = rgb565(40, 40, 46);
    s_grid_col[GRID_CENTER] = rgb565(75, 75, 82);

    // Gamma-corrected brightness ramp: a linear v/255 mix reads as dim in the
    // mid tones (perceived brightness isn't linear, and this is also what the
    // AA edges and decaying trail pixels sample), so bias it toward brighter
    // with a <1 gamma — full brightness (v=255) is unaffected either way.
    for (int v = 0; v < 256; v++) {
        float g = powf((float)v / 255.0f, 0.6f);
        s_pal_l[v] = rgb565((uint8_t)(230.0f * g), (uint8_t)(200.0f * g), (uint8_t)(40.0f * g));
        s_pal_r[v] = rgb565((uint8_t)(60.0f * g),  (uint8_t)(200.0f * g), (uint8_t)(230.0f * g));
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

void mode_oscope_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    // Fade the trails.
    uint8_t fade = s_trail_levels[s_trail_level];
    for (uint32_t i = 0; i < sizeof(s_buf_l); i++) {
        s_buf_l[i] = (uint8_t)((s_buf_l[i] * fade) >> 8);
        s_buf_r[i] = (uint8_t)((s_buf_r[i] * fade) >> 8);
    }

    // Plot this sweep's min/max envelope, edge-antialiased. Each column is
    // bridged to the previous one's span (extended to touch it if they don't
    // already overlap) so the trace reads as a connected line rather than
    // independent per-column dashes wherever the signal moves fast between
    // columns or the trigger point lands on a slightly different sample from
    // one frame to the next.
    float prev_top_l = 0.0f, prev_bot_l = 0.0f;
    float prev_top_r = 0.0f, prev_bot_r = 0.0f;
    for (int x = 0; x < HW; x++) {
        float top_l = (float)BASELINE - (float)vs->scope_max_l[x] * AMP_SCALE;
        float bot_l = (float)BASELINE - (float)vs->scope_min_l[x] * AMP_SCALE;
        float top_r = (float)BASELINE - (float)vs->scope_max_r[x] * AMP_SCALE;
        float bot_r = (float)BASELINE - (float)vs->scope_min_r[x] * AMP_SCALE;
        if (top_l > bot_l) { float t = top_l; top_l = bot_l; bot_l = t; }
        if (top_r > bot_r) { float t = top_r; top_r = bot_r; bot_r = t; }

        if (x > 0) {
            if (top_l > prev_bot_l)      top_l = prev_bot_l;
            else if (bot_l < prev_top_l) bot_l = prev_top_l;
            if (top_r > prev_bot_r)      top_r = prev_bot_r;
            else if (bot_r < prev_top_r) bot_r = prev_top_r;
        }
        plot_span(s_buf_l, x, top_l, bot_l);
        plot_span(s_buf_r, x, top_r, bot_r);
        prev_top_l = top_l; prev_bot_l = bot_l;
        prev_top_r = top_r; prev_bot_r = bot_r;
    }

    // Fused blit (portrait scan; lx = r, ly = 171 - c; bilinear 2x2 upscale)
    // — same pattern as mode_starfield.c / mode_plasma.c but blended rather
    // than nearest-neighbour doubled. Grid shows through only where both
    // trails are dark (checked against the nearest-neighbour tap, so grid
    // lines stay crisp); otherwise the (additively blended) trace colour
    // wins.
    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        int hx_m = (int)(r >> 1);
        int hx_o = (r & 1) ? hx_m + 1 : hx_m - 1;
        hx_o = clampi(hx_o, 0, HW - 1);
        bool vdiv = is_vdiv(hx_m);
        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            uint32_t d = (uint32_t)(171 - c);
            int hy_m = (int)(d >> 1);
            int hy_o = (d & 1) ? hy_m + 1 : hy_m - 1;
            hy_o = clampi(hy_o, 0, HH - 1);

            int bl = bilerp4(s_buf_l, hx_m, hx_o, hy_m, hy_o);
            int br = bilerp4(s_buf_r, hx_m, hx_o, hy_m, hy_o);
            uint16_t px;
            if (bl == 0 && br == 0) px = s_grid_col[grid_at(vdiv, hy_m)];
            else                    px = blend565(s_pal_l[bl], s_pal_r[br]);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
