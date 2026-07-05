#include "ui/mode_lufs.h"
#include "config.h"
#include "dsp/loudness.h"

#include <stdio.h>
#include <math.h>

// ===========================================================================
// LUFS view: EBU R128 loudness metering.
//
// Left: momentary (M) and short-term (S) horizontal bar meters on a
// -36..0 LUFS scale with the -14 LUFS streaming target marked. Right:
// integrated loudness (big), loudness range and max true peak. Long-press
// anywhere on the view resets integrated/LRA/true-peak.
// ===========================================================================

// Bar geometry
#define SCALE_MIN   (-36.0f)
#define SCALE_MAX   (0.0f)
#define BAR_X       30
#define BAR_W       380
#define BAR_H       30
#define BAR_M_Y     22
#define BAR_S_Y     78
#define TICKS_Y     (BAR_S_Y + BAR_H + 6)

// Loudness zones (LUFS)
#define TARGET_LUFS   (-14.0f)   // streaming target marker
#define HOT_LUFS      (-9.0f)

// Right panel
#define RP_X        432

static lv_obj_t *s_bar_m, *s_bar_s;
static lv_obj_t *s_val_m, *s_val_s;
static lv_obj_t *s_val_i, *s_val_lra, *s_val_tp;

static float s_prev_m = 1e9f, s_prev_s = 1e9f, s_prev_i = 1e9f;
static float s_prev_lra = -1.0f, s_prev_tp = 1e9f;

static int lufs_to_px(float l) {
    if (l < SCALE_MIN) l = SCALE_MIN;
    if (l > SCALE_MAX) l = SCALE_MAX;
    return (int)((l - SCALE_MIN) / (SCALE_MAX - SCALE_MIN) * BAR_W + 0.5f);
}

static lv_color_t zone_color(float l) {
    if (l >= HOT_LUFS)    return lv_color_make(224, 70, 50);    // too hot
    if (l >= TARGET_LUFS) return lv_color_make(235, 180, 60);   // above target
    if (l >= -23.0f)      return lv_color_make(80, 200, 110);   // healthy
    return lv_color_make(70, 150, 170);                          // quiet
}

static void plain(lv_obj_t *o, lv_color_t c) {
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col,
                       int x, int y, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void long_press_cb(lv_event_t *e) {
    (void)e;
    loudness_request_reset();
    // Swallow the rest of the press so the release doesn't also fire the
    // mode-switch tap handler.
    lv_indev_wait_release(lv_indev_get_act());
}

static void make_bar(lv_obj_t *parent, int y, const char *tag,
                     lv_obj_t **bar_out, lv_obj_t **val_out) {
    lv_color_t dim = lv_color_make(140, 138, 128);

    label(parent, &lv_font_montserrat_14, dim, 8, y + 8, tag);

    lv_obj_t *track = lv_obj_create(parent);
    plain(track, lv_color_make(26, 26, 28));
    lv_obj_set_size(track, BAR_W, BAR_H);
    lv_obj_set_pos(track, BAR_X, y);

    lv_obj_t *bar = lv_obj_create(parent);
    plain(bar, zone_color(SCALE_MIN));
    lv_obj_set_size(bar, 1, BAR_H);
    lv_obj_set_pos(bar, BAR_X, y);
    *bar_out = bar;

    // Numeric readout lives INSIDE the right end of the track so it can't
    // collide with the right-hand panel.
    lv_obj_t *v = label(parent, &lv_font_montserrat_14, lv_color_white(),
                        BAR_X + BAR_W - 68, y + 8, "-");
    lv_obj_set_width(v, 60);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    *val_out = v;
}

lv_obj_t *mode_lufs_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t dim = lv_color_make(140, 138, 128);
    lv_color_t faint = lv_color_make(70, 70, 74);

    make_bar(root, BAR_M_Y, "M", &s_bar_m, &s_val_m);
    make_bar(root, BAR_S_Y, "S", &s_bar_s, &s_val_s);

    // Scale ticks + labels below the S bar.
    static const float ticks[] = { -36, -30, -23, -18, -14, -9, -6, -3, 0 };
    char buf[8];
    for (unsigned i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        int x = BAR_X + lufs_to_px(ticks[i]);
        lv_obj_t *t = lv_obj_create(root);
        plain(t, faint);
        lv_obj_set_size(t, 1, 6);
        lv_obj_set_pos(t, x, TICKS_Y);
        snprintf(buf, sizeof(buf), "%d", (int)ticks[i]);
        lv_obj_t *l = label(root, &lv_font_montserrat_14, dim, 0, TICKS_Y + 8, buf);
        lv_obj_update_layout(l);
        lv_obj_set_x(l, x - lv_obj_get_width(l) / 2);
    }

    // Target marker line (-14 LUFS) across both bars.
    lv_obj_t *tgt = lv_obj_create(root);
    plain(tgt, lv_color_make(80, 200, 110));
    lv_obj_set_size(tgt, 2, (BAR_S_Y + BAR_H) - BAR_M_Y);
    lv_obj_set_pos(tgt, BAR_X + lufs_to_px(TARGET_LUFS), BAR_M_Y);

    // Right panel: integrated (big), LRA, true peak.
    label(root, &lv_font_montserrat_14, dim, RP_X, 10, "INTEGRATED  LUFS");
    s_val_i = label(root, &lv_font_montserrat_32, lv_color_white(), RP_X, 28, "-");

    label(root, &lv_font_montserrat_14, dim, RP_X, 76, "RANGE");
    s_val_lra = label(root, &lv_font_montserrat_20, lv_color_white(), RP_X, 92, "-");

    label(root, &lv_font_montserrat_14, dim, RP_X + 104, 76, "TRUE PEAK");
    s_val_tp = label(root, &lv_font_montserrat_20, lv_color_white(), RP_X + 104, 92, "-");

    label(root, &lv_font_montserrat_14, faint, RP_X, AV_DISP_H - 24, "hold to reset");

    lv_obj_add_event_cb(root, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    return root;
}

static void fmt_lufs(char *buf, size_t n, float v, const char *suffix) {
    if (v <= -99.0f) snprintf(buf, n, "-%s", suffix);
    else             snprintf(buf, n, "%.1f%s", (double)v, suffix);
}

void mode_lufs_update(const VisualizerState *vs) {
    char buf[24];

    if (vs->lufs_m != s_prev_m) {
        s_prev_m = vs->lufs_m;
        lv_obj_set_width(s_bar_m, LV_MAX(1, lufs_to_px(vs->lufs_m)));
        lv_obj_set_style_bg_color(s_bar_m, zone_color(vs->lufs_m), 0);
        fmt_lufs(buf, sizeof(buf), vs->lufs_m, "");
        lv_label_set_text(s_val_m, buf);
    }
    if (vs->lufs_s != s_prev_s) {
        s_prev_s = vs->lufs_s;
        lv_obj_set_width(s_bar_s, LV_MAX(1, lufs_to_px(vs->lufs_s)));
        lv_obj_set_style_bg_color(s_bar_s, zone_color(vs->lufs_s), 0);
        fmt_lufs(buf, sizeof(buf), vs->lufs_s, "");
        lv_label_set_text(s_val_s, buf);
    }
    if (vs->lufs_i != s_prev_i) {
        s_prev_i = vs->lufs_i;
        fmt_lufs(buf, sizeof(buf), vs->lufs_i, "");
        lv_label_set_text(s_val_i, buf);
        lv_obj_set_style_text_color(s_val_i, zone_color(vs->lufs_i), 0);
    }
    if (vs->lra != s_prev_lra) {
        s_prev_lra = vs->lra;
        snprintf(buf, sizeof(buf), "%.1f LU", (double)vs->lra);
        lv_label_set_text(s_val_lra, buf);
    }
    if (vs->tp_db != s_prev_tp) {
        s_prev_tp = vs->tp_db;
        if (vs->tp_db <= -99.0f) snprintf(buf, sizeof(buf), "-");
        else snprintf(buf, sizeof(buf), "%.1f dB", (double)vs->tp_db);
        lv_label_set_text(s_val_tp, buf);
        // Inter-sample overs territory: warn red at/above -1 dBTP.
        lv_obj_set_style_text_color(s_val_tp,
            (vs->tp_db >= -1.0f) ? lv_color_make(224, 70, 50) : lv_color_white(), 0);
    }
}
