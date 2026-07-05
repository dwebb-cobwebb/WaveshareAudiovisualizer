#include "ui/mode_producer.h"
#include "config.h"

// Layout (640 x 172 landscape)
#define SPEC_TOP     6
#define SPEC_BOT     150
#define SPEC_H       (SPEC_BOT - SPEC_TOP)
#define SLOT_W       (AV_SPECTRUM_W / AV_NUM_BANDS)
#define BAR_W        (SLOT_W - 5)

#define MET_X        (AV_SPECTRUM_W + 6)
#define MET_W        24
#define MET_GAP      8
#define MET_TOP      6
#define MET_BOT      150
#define MET_H        (MET_BOT - MET_TOP)
#define CLIP_H       8

#define PHASE_Y      160
#define PHASE_H      8
#define PHASE_X      (AV_SPECTRUM_W + 6)
#define PHASE_W      (AV_DISP_W - PHASE_X - 6)

static lv_obj_t *s_bars[AV_NUM_BANDS];
static lv_obj_t *s_peaks[AV_NUM_BANDS];
static lv_obj_t *s_met_l, *s_met_r;
static lv_obj_t *s_clip_l, *s_clip_r;
static lv_obj_t *s_phase_track, *s_phase_dot;

// Previous values — only call LVGL when something actually changes so we
// don't dirty the display every frame at 60 Hz.
static float s_prev_bands[AV_NUM_BANDS];
static float s_prev_peaks[AV_NUM_BANDS];
static float s_prev_peak_l = -1.f, s_prev_peak_r = -1.f;
static bool  s_prev_clip_l = false, s_prev_clip_r = false;
static float s_prev_corr = -2.f;   // out-of-range sentinel → forces first draw

static lv_color_t level_color(float v) {
    // green -> yellow -> red
    uint8_t r, g;
    if (v < 0.5f) { r = (uint8_t)(2.0f * v * 255); g = 255; }
    else          { r = 255; g = (uint8_t)((1.0f - v) * 2.0f * 255); }
    return lv_color_make(r, g, 40);
}

static void plain(lv_obj_t *o, lv_color_t c) {
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    // Display-only: never swallow taps meant for the mode-switch handler.
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *mode_producer_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    for (int b = 0; b < AV_NUM_BANDS; b++) {
        int x = b * SLOT_W + 2;

        lv_obj_t *bar = lv_obj_create(root);
        plain(bar, level_color(0.0f));
        lv_obj_set_size(bar, BAR_W, 1);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, x, -(AV_DISP_H - SPEC_BOT));
        s_bars[b] = bar;

        lv_obj_t *pk = lv_obj_create(root);
        plain(pk, lv_color_white());
        lv_obj_set_size(pk, BAR_W, 2);
        lv_obj_align(pk, LV_ALIGN_BOTTOM_LEFT, x, -(AV_DISP_H - SPEC_BOT));
        s_peaks[b] = pk;

        s_prev_bands[b] = 0.f;
        s_prev_peaks[b] = 0.f;
    }

    // Stereo peak meters (L, R)
    s_met_l = lv_obj_create(root); plain(s_met_l, lv_palette_main(LV_PALETTE_GREEN));
    lv_obj_set_size(s_met_l, MET_W, 1);
    lv_obj_align(s_met_l, LV_ALIGN_BOTTOM_LEFT, MET_X, -(AV_DISP_H - MET_BOT));

    s_met_r = lv_obj_create(root); plain(s_met_r, lv_palette_main(LV_PALETTE_GREEN));
    lv_obj_set_size(s_met_r, MET_W, 1);
    lv_obj_align(s_met_r, LV_ALIGN_BOTTOM_LEFT, MET_X + MET_W + MET_GAP, -(AV_DISP_H - MET_BOT));

    // Clip indicators (top of each meter)
    s_clip_l = lv_obj_create(root); plain(s_clip_l, lv_color_make(40, 40, 40));
    lv_obj_set_size(s_clip_l, MET_W, CLIP_H);
    lv_obj_align(s_clip_l, LV_ALIGN_TOP_LEFT, MET_X, MET_TOP - CLIP_H - 2);

    s_clip_r = lv_obj_create(root); plain(s_clip_r, lv_color_make(40, 40, 40));
    lv_obj_set_size(s_clip_r, MET_W, CLIP_H);
    lv_obj_align(s_clip_r, LV_ALIGN_TOP_LEFT, MET_X + MET_W + MET_GAP, MET_TOP - CLIP_H - 2);

    // Phase correlation bar (-1 left .. +1 right)
    s_phase_track = lv_obj_create(root); plain(s_phase_track, lv_color_make(30, 30, 30));
    lv_obj_set_size(s_phase_track, PHASE_W, PHASE_H);
    lv_obj_align(s_phase_track, LV_ALIGN_TOP_LEFT, PHASE_X, PHASE_Y);

    s_phase_dot = lv_obj_create(root); plain(s_phase_dot, lv_palette_main(LV_PALETTE_CYAN));
    lv_obj_set_size(s_phase_dot, 6, PHASE_H);
    lv_obj_align(s_phase_dot, LV_ALIGN_TOP_LEFT, PHASE_X + PHASE_W / 2 - 3, PHASE_Y);

    return root;
}

void mode_producer_update(const VisualizerState *vs) {
    for (int b = 0; b < AV_NUM_BANDS; b++) {
        if (vs->bands[b] != s_prev_bands[b]) {
            s_prev_bands[b] = vs->bands[b];
            int h = (int)(vs->bands[b] * SPEC_H);
            if (h < 1) h = 1;
            lv_obj_set_height(s_bars[b], h);
            lv_obj_set_style_bg_color(s_bars[b], level_color(vs->bands[b]), 0);
        }
        if (vs->band_peak[b] != s_prev_peaks[b]) {
            s_prev_peaks[b] = vs->band_peak[b];
            int ph = (int)(vs->band_peak[b] * SPEC_H);
            lv_obj_align(s_peaks[b], LV_ALIGN_BOTTOM_LEFT,
                         b * SLOT_W + 2, -(AV_DISP_H - SPEC_BOT) - ph);
        }
    }

    if (vs->peak_l != s_prev_peak_l) {
        s_prev_peak_l = vs->peak_l;
        int hl = (int)(vs->peak_l * MET_H); if (hl < 1) hl = 1;
        lv_obj_set_height(s_met_l, hl);
        lv_obj_set_style_bg_color(s_met_l, level_color(vs->peak_l), 0);
    }
    if (vs->peak_r != s_prev_peak_r) {
        s_prev_peak_r = vs->peak_r;
        int hr = (int)(vs->peak_r * MET_H); if (hr < 1) hr = 1;
        lv_obj_set_height(s_met_r, hr);
        lv_obj_set_style_bg_color(s_met_r, level_color(vs->peak_r), 0);
    }

    if (vs->clip_l != s_prev_clip_l) {
        s_prev_clip_l = vs->clip_l;
        lv_obj_set_style_bg_color(s_clip_l,
            vs->clip_l ? lv_palette_main(LV_PALETTE_RED) : lv_color_make(40, 40, 40), 0);
    }
    if (vs->clip_r != s_prev_clip_r) {
        s_prev_clip_r = vs->clip_r;
        lv_obj_set_style_bg_color(s_clip_r,
            vs->clip_r ? lv_palette_main(LV_PALETTE_RED) : lv_color_make(40, 40, 40), 0);
    }

    if (vs->correlation != s_prev_corr) {
        s_prev_corr = vs->correlation;
        int dot = (int)((vs->correlation * 0.5f + 0.5f) * (PHASE_W - 6));
        lv_obj_align(s_phase_dot, LV_ALIGN_TOP_LEFT, PHASE_X + dot, PHASE_Y);
    }
}
