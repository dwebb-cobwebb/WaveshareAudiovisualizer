#include "ui/mode_vibe.h"
#include "config.h"

#define VU_SIZE   150
#define VU_L_X    6
#define VU_R_X    (VU_L_X + VU_SIZE + 8)

#define BAR_X0    (VU_R_X + VU_SIZE + 8)
#define BAR_AREA  (AV_DISP_W - BAR_X0 - 6)
#define BAR_SLOT  (BAR_AREA / AV_NUM_BANDS)
#define BAR_W     (BAR_SLOT - 3)
#define BAR_TOP   10
#define BAR_BOT   150
#define BAR_H     (BAR_BOT - BAR_TOP)

// VU ballistics (mimic ~300ms integration)
#define VU_ATTACK 0.30f
#define VU_DECAY  0.06f

static lv_obj_t *s_vu_l, *s_vu_r;
static lv_meter_indicator_t *s_ind_l, *s_ind_r;
static lv_obj_t *s_bars[AV_NUM_BANDS];
static float s_needle_l, s_needle_r;

static lv_color_t neon(float v) {
    // cyan -> magenta gradient by height
    uint8_t r = (uint8_t)(v * 255);
    uint8_t b = 255;
    uint8_t g = (uint8_t)((1.0f - v) * 200);
    return lv_color_make(r, g, b);
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

static lv_obj_t *make_vu(lv_obj_t *parent, int x, lv_meter_indicator_t **ind) {
    lv_obj_t *m = lv_meter_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, VU_SIZE, VU_SIZE);
    lv_obj_align(m, LV_ALIGN_TOP_LEFT, x, 8);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);

    lv_meter_scale_t *scale = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, scale, 0, 100, 120, 210); // 120deg arc, rotated
    lv_meter_set_scale_ticks(m, scale, 11, 2, 8, lv_color_white());
    lv_meter_set_scale_major_ticks(m, scale, 5, 3, 12, lv_color_white(), 10);

    // Red "over" zone near the top end.
    lv_meter_indicator_t *zone = lv_meter_add_arc(m, scale, 4, lv_palette_main(LV_PALETTE_RED), 0);
    lv_meter_set_indicator_start_value(m, zone, 85);
    lv_meter_set_indicator_end_value(m, zone, 100);

    *ind = lv_meter_add_needle_line(m, scale, 3, lv_palette_main(LV_PALETTE_AMBER), -8);
    return m;
}

lv_obj_t *mode_vibe_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_vu_l = make_vu(root, VU_L_X, &s_ind_l);
    s_vu_r = make_vu(root, VU_R_X, &s_ind_r);

    for (int b = 0; b < AV_NUM_BANDS; b++) {
        lv_obj_t *bar = lv_obj_create(root);
        plain(bar, neon(0.0f));
        lv_obj_set_size(bar, BAR_W, 1);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, BAR_X0 + b * BAR_SLOT,
                     -(AV_DISP_H - BAR_BOT));
        s_bars[b] = bar;
    }

    s_needle_l = s_needle_r = 0.0f;
    return root;
}

void mode_vibe_update(const VisualizerState *vs) {
    float tl = vs->rms_l * 100.0f; if (tl > 100) tl = 100;
    float tr = vs->rms_r * 100.0f; if (tr > 100) tr = 100;
    s_needle_l += (tl - s_needle_l) * (tl > s_needle_l ? VU_ATTACK : VU_DECAY);
    s_needle_r += (tr - s_needle_r) * (tr > s_needle_r ? VU_ATTACK : VU_DECAY);
    lv_meter_set_indicator_value(s_vu_l, s_ind_l, (int32_t)s_needle_l);
    lv_meter_set_indicator_value(s_vu_r, s_ind_r, (int32_t)s_needle_r);

    for (int b = 0; b < AV_NUM_BANDS; b++) {
        int h = (int)(vs->bands[b] * BAR_H);
        if (h < 1) h = 1;
        lv_obj_set_height(s_bars[b], h);
        lv_obj_set_style_bg_color(s_bars[b], neon(vs->bands[b]), 0);
    }
}
