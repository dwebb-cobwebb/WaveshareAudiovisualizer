#include "ui/mode_vibe.h"
#include "config.h"
#include "assets/vu_assets.h"

#include <math.h>

// ===========================================================================
// Vibe view: a stereo pair of photorealistic black-face VU meters.
//
// The faces, needles and hub caps are pre-rendered images (see
// scripts/gen_vu_assets.py) with the authentic non-linear VU scale: the
// movement responds to rectified signal level and 0 VU sits at 71% of
// full-scale deflection, so dB marks land at 0.71 * 10^(dB/20) of the arc.
//
// Ballistics follow the ANSI C16.5 VU standard: a second-order underdamped
// system reaching 99% deflection in 300 ms with ~1% overshoot, simulated as
// a spring-damper in the LINEAR (rectifier-current) domain at 60 Hz.
// ===========================================================================

// Layout: two dials tile the 640x172 canvas edge-to-edge (faces are 320x172).
#define FACE_Y   0
#define L_X      0
#define R_X      (AV_DISP_W - VU_FACE_W)

// ANSI VU ballistics: 99% in 300 ms, ~1% overshoot.
#define VU_WN    21.0f     // natural frequency, rad/s
#define VU_ZETA  0.81f     // damping ratio
#define VU_DT    (1.0f / 60.0f)

// 0 VU calibration: RMS level (full-scale = 1.0) that reads 0 VU. -14 dBFS.
#define VU_REF_RMS   0.1995f

// Needle can peg slightly past +3.
#define VU_MAX_FRAC  1.03f

static lv_obj_t *s_needle_l, *s_needle_r;
static float s_pos_l, s_vel_l;
static float s_pos_r, s_vel_r;
static int16_t s_prev_a_l = INT16_MIN, s_prev_a_r = INT16_MIN;

// One spring-damper integration step toward `target` (arc fraction 0..1).
static float vu_step(float pos, float *vel, float target) {
    float acc = VU_WN * VU_WN * (target - pos) - 2.0f * VU_ZETA * VU_WN * (*vel);
    *vel += acc * VU_DT;
    pos += (*vel) * VU_DT;
    if (pos < 0.0f) { pos = 0.0f; if (*vel < 0.0f) *vel = 0.0f; }
    if (pos > VU_MAX_FRAC) { pos = VU_MAX_FRAC; if (*vel > 0.0f) *vel = 0.0f; }
    return pos;
}

// LVGL angle (0.1 deg units, 0..3600) for an arc fraction.
static int16_t vu_angle(float frac) {
    float deg = -VU_ARC_HALF_DEG + frac * (2.0f * VU_ARC_HALF_DEG);
    int a = (int)lroundf(deg * 10.0f);
    return (int16_t)((a + 3600) % 3600);
}

static void make_dial(lv_obj_t *parent, int x, const char *tag,
                      lv_obj_t **needle_out) {
    lv_obj_t *face = lv_img_create(parent);
    lv_img_set_src(face, &vu_face);
    lv_obj_set_pos(face, x, FACE_Y);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);

    // Channel tag, bottom-left of the face.
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_make(150, 146, 132), 0);
    lv_label_set_text(lbl, tag);
    lv_obj_set_pos(lbl, x + 16, FACE_Y + VU_FACE_H - 28);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    // Needle: rotated around its baked pivot, placed so the pivot lands on
    // the dial's pivot point.
    lv_obj_t *ndl = lv_img_create(parent);
    lv_img_set_src(ndl, &vu_needle);
    lv_obj_set_pos(ndl,
                   x + VU_PIVOT_X - VU_NEEDLE_PIVOT_X,
                   FACE_Y + VU_PIVOT_Y - VU_NEEDLE_PIVOT_Y);
    lv_img_set_pivot(ndl, VU_NEEDLE_PIVOT_X, VU_NEEDLE_PIVOT_Y);
    lv_img_set_antialias(ndl, true);
    lv_obj_clear_flag(ndl, LV_OBJ_FLAG_CLICKABLE);

    // Hub cap over the needle base.
    lv_obj_t *hub = lv_img_create(parent);
    lv_img_set_src(hub, &vu_hub);
    lv_obj_set_pos(hub,
                   x + VU_PIVOT_X - VU_HUB_D / 2,
                   FACE_Y + VU_PIVOT_Y - VU_HUB_D / 2);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    *needle_out = ndl;
}

lv_obj_t *mode_vibe_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    make_dial(root, L_X, "L", &s_needle_l);
    make_dial(root, R_X, "R", &s_needle_r);

    s_pos_l = s_vel_l = 0.0f;
    s_pos_r = s_vel_r = 0.0f;
    lv_img_set_angle(s_needle_l, vu_angle(0.0f));
    lv_img_set_angle(s_needle_r, vu_angle(0.0f));

    return root;
}

void mode_vibe_update(const VisualizerState *vs) {
    // Rectifier-domain target: fraction of full deflection (0 VU = 0.71).
    float tl = (vs->rms_l / VU_REF_RMS) * 0.71f;
    float tr = (vs->rms_r / VU_REF_RMS) * 0.71f;
    if (tl > VU_MAX_FRAC) tl = VU_MAX_FRAC;
    if (tr > VU_MAX_FRAC) tr = VU_MAX_FRAC;

    s_pos_l = vu_step(s_pos_l, &s_vel_l, tl);
    s_pos_r = vu_step(s_pos_r, &s_vel_r, tr);

    int16_t al = vu_angle(s_pos_l);
    int16_t ar = vu_angle(s_pos_r);
    if (al != s_prev_a_l) { s_prev_a_l = al; lv_img_set_angle(s_needle_l, al); }
    if (ar != s_prev_a_r) { s_prev_a_r = ar; lv_img_set_angle(s_needle_r, ar); }
}
