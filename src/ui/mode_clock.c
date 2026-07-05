#include "ui/mode_clock.h"
#include "config.h"
#include "util/wallclock.h"
#include "assets/clock_assets.h"

#include <stdio.h>

// ===========================================================================
// Clock view: traditional 7-segment LED clock.
//
// Digits are composed from pre-rendered glowing LED segment images
// (scripts/gen_clock_assets.py); unlit segments stay faintly visible like a
// real LED display's ghost segments, and the colon blinks at 1 Hz. Small
// date line ("SUN 5 JUL") underneath. Shown even when no audio is streaming
// and exempt from display auto-sleep. Time arrives over the CDC serial port
// (scripts/set_clock.ps1); until then the display reads "--:--".
// ===========================================================================

#define N_DIGITS   4
#define GHOST_OPA  16    // unlit-segment opacity
#define Y0         4     // top of the digit row
#define DIGIT_GAP  10
#define COLON_W    34

// Time occupies the left area; the date stacks in a column on the right.
#define DATE_COL_W  116
#define TIME_AREA_W (AV_DISP_W - DATE_COL_W)

// Segment bit order: A B C D E F G (bit 0..6)
#define SA 0x01
#define SB 0x02
#define SC 0x04
#define SD 0x08
#define SE 0x10
#define SF 0x20
#define SG 0x40

static const uint8_t SEGMASK[11] = {
    /*0*/ SA|SB|SC|SD|SE|SF,
    /*1*/ SB|SC,
    /*2*/ SA|SB|SD|SE|SG,
    /*3*/ SA|SB|SC|SD|SG,
    /*4*/ SB|SC|SF|SG,
    /*5*/ SA|SC|SD|SF|SG,
    /*6*/ SA|SC|SD|SE|SF|SG,
    /*7*/ SA|SB|SC,
    /*8*/ SA|SB|SC|SD|SE|SF|SG,
    /*9*/ SA|SB|SC|SD|SF|SG,
    /*10 = '-' */ SG,
};
#define GLYPH_DASH 10

// Per-segment placement within a digit cell: x, y of the segment SHAPE's
// top-left (the image adds SEG_PAD of glow bleed around it), horizontal flag.
typedef struct { int16_t x, y; bool horiz; } seg_pos_t;
static const seg_pos_t SEG_POS[7] = {
    { SEG_THICK / 2 + SEG_GAP, 0,                              true  }, // A
    { SEG_DIGIT_W - SEG_THICK, SEG_THICK / 2 + SEG_GAP,        false }, // B
    { SEG_DIGIT_W - SEG_THICK,
      (SEG_DIGIT_H - SEG_THICK) / 2 + SEG_THICK / 2 + SEG_GAP, false }, // C
    { SEG_THICK / 2 + SEG_GAP, SEG_DIGIT_H - SEG_THICK,        true  }, // D
    { 0, (SEG_DIGIT_H - SEG_THICK) / 2 + SEG_THICK / 2 + SEG_GAP, false }, // E
    { 0, SEG_THICK / 2 + SEG_GAP,                              false }, // F
    { SEG_THICK / 2 + SEG_GAP, (SEG_DIGIT_H - SEG_THICK) / 2,  true  }, // G
};

static lv_obj_t *s_seg[N_DIGITS][7];
static lv_obj_t *s_dot_top, *s_dot_bot;
static lv_obj_t *s_day_lbl, *s_num_lbl, *s_mon_lbl;
static uint8_t s_shown[N_DIGITS] = { 0xFF, 0xFF, 0xFF, 0xFF };
static int s_prev_sec = -1;
static int s_prev_yday = -1;
static bool s_prev_valid = true;   // force first render

static const char *DAYS[7]    = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *MONTHS[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static void set_glyph(int slot, uint8_t glyph) {
    if (s_shown[slot] == glyph) return;
    s_shown[slot] = glyph;
    uint8_t mask = SEGMASK[glyph];
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_img_opa(s_seg[slot][i],
                                 (mask & (1u << i)) ? LV_OPA_COVER : GHOST_OPA, 0);
    }
}

static void make_digit(lv_obj_t *parent, int slot, int cell_x) {
    for (int i = 0; i < 7; i++) {
        lv_obj_t *img = lv_img_create(parent);
        lv_img_set_src(img, SEG_POS[i].horiz ? &seg_h : &seg_v);
        lv_obj_set_pos(img, cell_x + SEG_POS[i].x - SEG_PAD,
                       Y0 + SEG_POS[i].y - SEG_PAD);
        lv_obj_set_style_img_opa(img, GHOST_OPA, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        s_seg[slot][i] = img;
    }
}

lv_obj_t *mode_clock_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    int total = 4 * SEG_DIGIT_W + 3 * DIGIT_GAP + COLON_W + 2 * DIGIT_GAP;
    int x = (TIME_AREA_W - total) / 2;

    make_digit(root, 0, x); x += SEG_DIGIT_W + DIGIT_GAP;
    make_digit(root, 1, x); x += SEG_DIGIT_W + DIGIT_GAP;

    int dot_x = x + (COLON_W - SEG_DOT_D) / 2 - SEG_PAD;
    s_dot_top = lv_img_create(root);
    lv_img_set_src(s_dot_top, &seg_dot);
    lv_obj_set_pos(s_dot_top, dot_x, Y0 + SEG_DIGIT_H / 3 - SEG_DOT_D / 2 - SEG_PAD);
    lv_obj_clear_flag(s_dot_top, LV_OBJ_FLAG_CLICKABLE);
    s_dot_bot = lv_img_create(root);
    lv_img_set_src(s_dot_bot, &seg_dot);
    lv_obj_set_pos(s_dot_bot, dot_x, Y0 + 2 * SEG_DIGIT_H / 3 - SEG_DOT_D / 2 - SEG_PAD);
    lv_obj_clear_flag(s_dot_bot, LV_OBJ_FLAG_CLICKABLE);
    x += COLON_W + DIGIT_GAP;

    make_digit(root, 2, x); x += SEG_DIGIT_W + DIGIT_GAP;
    make_digit(root, 3, x);

    // Date column: DAY / day-number / MONTH stacked on the right.
    lv_color_t dim = lv_color_make(150, 146, 132);
    struct { lv_obj_t **lbl; const lv_font_t *font; int y; } col[3] = {
        { &s_day_lbl, &lv_font_montserrat_20, 30 },
        { &s_num_lbl, &lv_font_montserrat_32, 66 },
        { &s_mon_lbl, &lv_font_montserrat_20, 116 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_label_create(root);
        lv_obj_set_style_text_font(l, col[i].font, 0);
        lv_obj_set_style_text_color(l, dim, 0);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_obj_set_width(l, DATE_COL_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, "");
        lv_obj_set_pos(l, TIME_AREA_W, col[i].y);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        *col[i].lbl = l;
    }

    return root;
}

void mode_clock_update(const VisualizerState *vs) {
    (void)vs;
    struct tm tm;
    bool valid = wallclock_get(&tm);

    if (!valid) {
        if (s_prev_valid) {
            s_prev_valid = false;
            for (int i = 0; i < N_DIGITS; i++) set_glyph(i, GLYPH_DASH);
            lv_obj_set_style_img_opa(s_dot_top, GHOST_OPA, 0);
            lv_obj_set_style_img_opa(s_dot_bot, GHOST_OPA, 0);
            lv_label_set_text(s_day_lbl, "SET");
            lv_label_set_text(s_num_lbl, "?");
            lv_label_set_text(s_mon_lbl, "TIME");
            s_prev_sec = -1;
            s_prev_yday = -1;
        }
        return;
    }
    s_prev_valid = true;

    if (tm.tm_sec == s_prev_sec && tm.tm_yday == s_prev_yday) return;
    s_prev_sec = tm.tm_sec;

    set_glyph(0, (uint8_t)(tm.tm_hour / 10));
    set_glyph(1, (uint8_t)(tm.tm_hour % 10));
    set_glyph(2, (uint8_t)(tm.tm_min / 10));
    set_glyph(3, (uint8_t)(tm.tm_min % 10));

    // Colon blinks at 1 Hz (lit on even seconds).
    lv_opa_t dot = (tm.tm_sec & 1) ? GHOST_OPA : LV_OPA_COVER;
    lv_obj_set_style_img_opa(s_dot_top, dot, 0);
    lv_obj_set_style_img_opa(s_dot_bot, dot, 0);

    if (tm.tm_yday != s_prev_yday) {
        s_prev_yday = tm.tm_yday;
        char buf[8];
        lv_label_set_text(s_day_lbl, DAYS[tm.tm_wday % 7]);
        snprintf(buf, sizeof(buf), "%d", tm.tm_mday);
        lv_label_set_text(s_num_lbl, buf);
        lv_label_set_text(s_mon_lbl, MONTHS[tm.tm_mon % 12]);
    }
}
