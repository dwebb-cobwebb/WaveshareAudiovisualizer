#include "ui/mode_spectrum.h"
#include "config.h"
#include "display/axs15231b.h"
#include "display/qspi_pio.h"

#include <math.h>
#include <stdio.h>

// ===========================================================================
// Spectrum view: smooth filled log-frequency curve with a peak-hold overlay,
// in the style of FabFilter Pro-Q's analyser — a continuous curve rather than
// discrete bars (that's the Producer view), colour-coded by mixing-relevant
// frequency zone, with a bold peak-hold line riding above it. Tap anywhere to
// drop a marker and read off the exact frequency there.
//
// Reuses the exact same bands[]/band_peak[] data the Producer view already
// computes (log-spaced, attack/decay ballistics already applied) — no new
// analyzer work and no VisualizerState growth. Band index is linear in log-
// frequency by construction (see analyzer.c's build_bands), so spreading the
// 31 bands evenly across the display width already gives a log-frequency
// x-axis; the curve between band centres is smoothstep-interpolated.
//
// Rendered fully procedurally (no stored framebuffer), like mode_plasma.c —
// only a couple of tiny lookup tables, so it costs effectively no static RAM,
// which matters after the goniometer saga taught us that lesson the hard way.
// The tap readout is a tiny baked-in 3x5 bitmap font drawn straight into the
// same pixel stream — this mode bypasses LVGL's own renderer entirely, so
// there's no text widget available to just drop on top.
// ===========================================================================

#define FRAME_MS    33
#define CHART_TOP   6
#define CHART_BOT   165
#define CHART_H     (CHART_BOT - CHART_TOP)

// Mixing-relevant frequency zones (low -> high), each with a muted "quiet"
// colour and a vivid "loud" colour; the fill gradient runs between them per
// column. Boundaries follow the common sub-bass/bass/low-mid/mid/high-mid/
// high split.
typedef struct {
    float   f_lo;
    uint8_t br, bg, bb;   // baseline (quiet) colour
    uint8_t tr, tg, tb;   // full-scale (loud) colour
} zone_t;

static const zone_t ZONES[6] = {
    {    20.0f,  15,10,18,  215,55,70  },   // sub-bass:  20-60 Hz   — deep red
    {    60.0f,  20,14,10,  225,130,50 },   // bass:      60-250 Hz  — orange
    {   250.0f,  20,20,10,  215,200,60 },   // low-mids:  250-500 Hz — yellow
    {   500.0f,  10,20,14,  70,215,105 },   // mids:      500 Hz-2 kHz — green
    {  2000.0f,  10,18,20,  60,200,220 },   // high-mids: 2-6 kHz    — cyan
    {  6000.0f,  14,14,26,  115,140,240 },  // highs:     6-20 kHz   — blue/violet
};
#define NUM_ZONES 6

// Shared gamma-corrected brightness curve (row -> 0..255), applied to
// whichever zone's quiet/loud colour pair is active at render time — one
// 172-byte table instead of a 2 KB+ full RGB LUT per zone.
static uint8_t  s_gamma_u8[AV_DISP_NATIVE_W];
static uint16_t s_grid_col;
static uint16_t s_peak_col;
static uint16_t s_marker_col;
static uint16_t s_text_bg, s_text_fg;
static int      s_grid_x[3];       // pixel columns for 100 Hz / 1 kHz / 10 kHz
static float    s_zone_cx[NUM_ZONES];   // pixel column at each zone's centre
static lv_obj_t *s_root;
static uint32_t s_last_frame_ms;

static int      s_tap_x = -1;       // tapped column, -1 = no marker
static char     s_tap_text[8];
static int8_t   s_tap_glyph[8];
static int      s_tap_text_len;
static int      s_tap_box_w;

// ---------------------------------------------------------------------------
// Tiny 3x5 dot-matrix font: digits, '.', and 'k' — just enough for a
// frequency readout like "1.2k" or "125". Each row is a 3-bit mask,
// MSB = leftmost column.
// ---------------------------------------------------------------------------
#define GLYPH_W     3
#define GLYPH_H     5
#define GLYPH_SCALE 3
#define GLYPH_GAP   2
#define TEXT_PAD    3
#define TEXT_X0     6
#define TEXT_Y0     6
#define TEXT_BOX_H  (GLYPH_H * GLYPH_SCALE + 2 * TEXT_PAD)

static const uint8_t FONT[12][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },   // 0
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   // 1
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   // 2
    { 0x7, 0x1, 0x7, 0x1, 0x7 },   // 3
    { 0x5, 0x5, 0x7, 0x1, 0x1 },   // 4
    { 0x7, 0x4, 0x7, 0x1, 0x7 },   // 5
    { 0x7, 0x4, 0x7, 0x5, 0x7 },   // 6
    { 0x7, 0x1, 0x1, 0x1, 0x1 },   // 7
    { 0x7, 0x5, 0x7, 0x5, 0x7 },   // 8
    { 0x7, 0x5, 0x7, 0x1, 0x7 },   // 9
    { 0x0, 0x0, 0x0, 0x0, 0x2 },   // .
    { 0x4, 0x5, 0x6, 0x5, 0x5 },   // k
};

static int glyph_index(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == '.') return 10;
    if (ch == 'k') return 11;
    return -1;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t scale565(uint16_t c, float k) {
    if (k <= 0.0f) return 0;
    if (k >= 1.0f) return c;
    int r = (int)(((c >> 11) & 0x1F) * k);
    int g = (int)(((c >> 5) & 0x3F) * k);
    int b = (int)((c & 0x1F) * k);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Blends a zone's quiet->loud colour pair using the shared brightness curve.
static uint16_t zone_color(int z, int ly) {
    int g8 = s_gamma_u8[ly];
    int r = ZONES[z].br + ((ZONES[z].tr - ZONES[z].br) * g8) / 255;
    int g = ZONES[z].bg + ((ZONES[z].tg - ZONES[z].bg) * g8) / 255;
    int b = ZONES[z].bb + ((ZONES[z].tb - ZONES[z].bb) * g8) / 255;
    return rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Pixel column for a given frequency, matching analyzer.c's build_bands() log
// mapping. Used for the three reference gridlines, the zone-boundary columns,
// and (inverted) to turn a tap column back into a frequency.
static int freq_to_x(float f) {
    float bp = (log10f(f) - log10f(AV_BAND_FREQ_LOW)) /
               (log10f(AV_BAND_FREQ_HIGH) - log10f(AV_BAND_FREQ_LOW));
    return (int)(bp * (float)(AV_DISP_W - 1) + 0.5f);
}

static float x_to_freq(int x) {
    float bp = (float)x / (float)(AV_DISP_W - 1);
    float log_lo = log10f(AV_BAND_FREQ_LOW);
    float log_hi = log10f(AV_BAND_FREQ_HIGH);
    return powf(10.0f, log_lo + bp * (log_hi - log_lo));
}

static void format_freq(float f, char *buf, int bufsize) {
    if (f < 1000.0f) {
        snprintf(buf, (size_t)bufsize, "%d", (int)(f + 0.5f));
    } else {
        float k = f / 1000.0f;
        if (k < 10.0f) snprintf(buf, (size_t)bufsize, "%.1fk", (double)k);
        else           snprintf(buf, (size_t)bufsize, "%dk", (int)(k + 0.5f));
    }
}

// Long-press clears the marker/readout (it otherwise persists across mode
// swipes, which is deliberate); swallow the rest of the press so the release
// doesn't also fire a fresh tap_cb at the same spot (same pattern as the LUFS
// view's long-press reset).
static void long_press_cb(lv_event_t *e) {
    (void)e;
    s_tap_x = -1;
    lv_indev_wait_release(lv_indev_get_act());
}

static void tap_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_tap_x = p.x;
    if (s_tap_x < 0) s_tap_x = 0;
    if (s_tap_x > AV_DISP_W - 1) s_tap_x = AV_DISP_W - 1;

    format_freq(x_to_freq(s_tap_x), s_tap_text, (int)sizeof(s_tap_text));
    s_tap_text_len = 0;
    for (int i = 0; s_tap_text[i] != '\0' && i < (int)sizeof(s_tap_glyph); i++) {
        s_tap_glyph[i] = (int8_t)glyph_index(s_tap_text[i]);
        s_tap_text_len++;
    }
    s_tap_box_w = 2 * TEXT_PAD + s_tap_text_len * (GLYPH_W * GLYPH_SCALE)
                + (s_tap_text_len - 1) * GLYPH_GAP;
}

lv_obj_t *mode_spectrum_create(lv_obj_t *parent) {
    s_grid_col   = rgb565(40, 40, 46);
    s_peak_col   = rgb565(235, 245, 255);
    s_marker_col = rgb565(255, 255, 255);
    s_text_bg    = rgb565(20, 20, 24);
    s_text_fg    = rgb565(240, 240, 245);

    // Gamma-corrected quiet->loud blend factor per row (same reasoning as the
    // oscilloscope/goniometer brightness ramps: a linear mix reads as dim in
    // the mid tones). Shared across all zones — only the colour pair being
    // blended between changes.
    for (int ly = 0; ly < AV_DISP_NATIVE_W; ly++) {
        float frac = ((float)CHART_BOT - (float)ly) / (float)CHART_H;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        s_gamma_u8[ly] = (uint8_t)(powf(frac, 0.6f) * 255.0f + 0.5f);
    }

    s_grid_x[0] = freq_to_x(100.0f);
    s_grid_x[1] = freq_to_x(1000.0f);
    s_grid_x[2] = freq_to_x(10000.0f);

    // Each zone's centre in pixel space, as the midpoint of its own x-range —
    // used to crossfade between adjacent zones instead of a hard cut at the
    // boundary.
    {
        float bx[NUM_ZONES + 1];
        bx[0] = 0.0f;
        for (int z = 1; z < NUM_ZONES; z++) bx[z] = (float)freq_to_x(ZONES[z].f_lo);
        bx[NUM_ZONES] = (float)(AV_DISP_W - 1);
        for (int z = 0; z < NUM_ZONES; z++) s_zone_cx[z] = (bx[z] + bx[z + 1]) * 0.5f;
    }

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_root, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    return s_root;
}

void mode_spectrum_update(const VisualizerState *vs) {
    if (lv_tick_elaps(s_last_frame_ms) < FRAME_MS) return;
    s_last_frame_ms = lv_tick_get();

    axs_stream_begin();
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        // Band position is linear in log-frequency by construction, so a
        // straight lerp across pixel columns reproduces a log-frequency axis.
        float bf = ((float)r / (float)(AV_DISP_W - 1)) * (float)(AV_NUM_BANDS - 1);
        int b0 = (int)bf;
        if (b0 < 0) b0 = 0;
        if (b0 > AV_NUM_BANDS - 2) b0 = AV_NUM_BANDS - 2;
        float t = bf - (float)b0;
        float ts = t * t * (3.0f - 2.0f * t);   // smoothstep, cheaper than cosine

        float height = vs->bands[b0]      + (vs->bands[b0 + 1]      - vs->bands[b0])      * ts;
        float peak   = vs->band_peak[b0]  + (vs->band_peak[b0 + 1]  - vs->band_peak[b0])  * ts;
        if (height < 0.0f) height = 0.0f; if (height > 1.0f) height = 1.0f;
        if (peak   < 0.0f) peak   = 0.0f; if (peak   > 1.0f) peak   = 1.0f;

        float boundary = (float)CHART_BOT - height * (float)CHART_H;
        float peak_ly  = (float)CHART_BOT - peak   * (float)CHART_H;
        int row_top = (int)floorf(boundary);
        float frac = boundary - (float)row_top;

        bool vdiv = ((int)r == s_grid_x[0] || (int)r == s_grid_x[1] || (int)r == s_grid_x[2]);
        bool marker = (s_tap_x >= 0 && (int)r == s_tap_x);

        // Crossfade between the two nearest zone centres rather than a hard
        // cut at the boundary — same smoothstep idea as the curve itself.
        float rx = (float)r;
        int z0 = 0;
        for (int i = 1; i < NUM_ZONES; i++) { if (rx >= s_zone_cx[i]) z0 = i; }
        int z1 = (z0 < NUM_ZONES - 1) ? z0 + 1 : z0;
        float zt = 0.0f;
        if (z1 != z0) {
            zt = (rx - s_zone_cx[z0]) / (s_zone_cx[z1] - s_zone_cx[z0]);
            if (zt < 0.0f) zt = 0.0f;
            if (zt > 1.0f) zt = 1.0f;
            zt = zt * zt * (3.0f - 2.0f * zt);
        }

        bool in_text_x = (s_tap_x >= 0) && ((int)r >= TEXT_X0) && ((int)r < TEXT_X0 + s_tap_box_w);

        for (int32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
            int ly = 171 - c;
            float fill_cov = (ly > row_top) ? 1.0f : (ly == row_top ? (1.0f - frac) : 0.0f);

            uint16_t px;
            if (fill_cov > 0.0f) {
                uint16_t zc = (z1 != z0) ? lerp565(zone_color(z0, ly), zone_color(z1, ly), zt)
                                         : zone_color(z0, ly);
                px = scale565(zc, fill_cov);
            } else if (vdiv && ly >= CHART_TOP && ly <= CHART_BOT) {
                px = s_grid_col;
            } else {
                px = 0;
            }

            // Flat-topped core (solid for the middle ~3px) with a 1px
            // antialiased taper on each side — reads as a bold, crisp line
            // rather than the soft triangular falloff a pure linear covers
            // gives (same "solid core + AA edge" idea as the oscilloscope's
            // plot_span).
            float pdist = fabsf((float)ly - peak_ly);
            float pcov = (pdist <= 1.0f) ? 1.0f : (2.0f - pdist);
            if (pcov > 0.0f) {
                if (pcov > 1.0f) pcov = 1.0f;
                px = lerp565(px, s_peak_col, pcov);
            }

            if (marker) px = s_marker_col;

            // Frequency readout: a small backing box with a tiny bitmap font,
            // drawn last (highest priority) so it stays legible over
            // whatever fill/peak/marker colour is underneath.
            if (in_text_x && ly >= TEXT_Y0 && ly < TEXT_Y0 + TEXT_BOX_H) {
                int lx_rel = (int)r - TEXT_X0 - TEXT_PAD;
                int ly_rel = ly - TEXT_Y0 - TEXT_PAD;
                const int glyph_stride = GLYPH_W * GLYPH_SCALE + GLYPH_GAP;
                if (lx_rel < 0 || ly_rel < 0 || ly_rel >= GLYPH_H * GLYPH_SCALE) {
                    px = s_text_bg;
                } else {
                    int gi = lx_rel / glyph_stride;
                    int within = lx_rel - gi * glyph_stride;
                    if (gi >= s_tap_text_len || within >= GLYPH_W * GLYPH_SCALE) {
                        px = s_text_bg;
                    } else {
                        int col_in_glyph = within / GLYPH_SCALE;
                        int row_in_glyph = ly_rel / GLYPH_SCALE;
                        int8_t glyph = s_tap_glyph[gi];
                        bool lit = glyph >= 0 &&
                                   ((FONT[glyph][row_in_glyph] >> (GLYPH_W - 1 - col_in_glyph)) & 1);
                        px = lit ? s_text_fg : s_text_bg;
                    }
                }
            }

            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px >> 8) << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(px & 0xFF) << 24);
        }
    }
    axs_stream_end();
}
