#include "ui/mode_clock.h"
#include "config.h"
#include "util/wallclock.h"

#include <stdio.h>
#include <string.h>

// ===========================================================================
// Clock view: big digital time, small date ("SUN 5 JUL").
//
// Shown even when no audio is streaming, and exempt from display auto-sleep
// (a desk clock should stay up). Time is pushed from the host over the CDC
// serial port — run scripts/set_clock.ps1 (or add it as a logon task).
// ===========================================================================

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_hint_lbl;
static int s_prev_min = -1;
static bool s_prev_valid = false;

static const char *DAYS[7]   = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *MONTHS[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

lv_obj_t *mode_clock_create(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, AV_DISP_W, AV_DISP_H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_time_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(s_time_lbl, 4, 0);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -22);
    lv_obj_clear_flag(s_time_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_date_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(s_date_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_make(150, 146, 132), 0);
    lv_obj_set_style_text_letter_space(s_date_lbl, 3, 0);
    lv_label_set_text(s_date_lbl, "");
    lv_obj_align(s_date_lbl, LV_ALIGN_CENTER, 0, 28);
    lv_obj_clear_flag(s_date_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_hint_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint_lbl, lv_color_make(90, 90, 94), 0);
    lv_label_set_text(s_hint_lbl, "set time: scripts/set_clock.ps1");
    lv_obj_align(s_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(s_hint_lbl, LV_OBJ_FLAG_CLICKABLE);

    return root;
}

void mode_clock_update(const VisualizerState *vs) {
    (void)vs;
    struct tm tm;
    bool valid = wallclock_get(&tm);

    if (valid != s_prev_valid) {
        s_prev_valid = valid;
        if (valid) lv_obj_add_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        s_prev_min = -1;
    }
    if (!valid) return;

    int now_min = tm.tm_hour * 60 + tm.tm_min;
    if (now_min == s_prev_min) return;
    s_prev_min = now_min;

    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_time_lbl, buf);
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -22);

    snprintf(buf, sizeof(buf), "%s %d %s",
             DAYS[tm.tm_wday % 7], tm.tm_mday, MONTHS[tm.tm_mon % 12]);
    lv_label_set_text(s_date_lbl, buf);
    lv_obj_align(s_date_lbl, LV_ALIGN_CENTER, 0, 28);
}
