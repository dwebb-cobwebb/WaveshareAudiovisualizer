#include "ui/ui_common.h"
#include "ui/mode_producer.h"
#include "ui/mode_vibe.h"
#include "ui/mode_lufs.h"
#include "ui/mode_tunnel.h"
#include "ui/mode_clock.h"
#include "dsp/vis_state.h"
#include "usb/usb_audio.h"
#include "display/axs15231b.h"
#include "config.h"
#include "tusb.h"

static lv_obj_t *s_mode_obj[AV_MODE_COUNT];
static AppMode   s_mode = AV_MODE_PRODUCER;
static lv_obj_t *s_usb_label;
static lv_obj_t *s_status_label;
static bool      s_streaming = false;

// Display sleep: the panel backlight can be fully blanked in software while
// the touch controller stays alive to wake it. Sleeps automatically after
// AV_DISPLAY_TIMEOUT_MS without streaming, wakes automatically when audio
// starts; swipe DOWN sleeps immediately, any touch wakes.
static bool     s_display_on = true;
static bool     s_was_streaming = false;
static uint32_t s_last_active_ms = 0;

static void display_set(bool on) {
    if (on == s_display_on) return;
    s_display_on = on;
    axs_backlight(on ? AV_BACKLIGHT_LEVEL : 0);
    if (on) s_last_active_ms = lv_tick_get();
}

static void apply_visibility(void) {
    for (int i = 0; i < AV_MODE_COUNT; i++) {
        // The clock is useful without audio; other modes need a stream.
        bool show = (i == (int)s_mode) &&
                    (s_streaming || i == AV_MODE_CLOCK);
        if (show) lv_obj_clear_flag(s_mode_obj[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag  (s_mode_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    // Idle diagnostics (waiting banner + status lines) only when nothing
    // else is on screen.
    if (s_streaming || s_mode == AV_MODE_CLOCK) {
        lv_obj_add_flag(s_usb_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_usb_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

AppMode ui_current_mode(void) { return s_mode; }

void ui_set_mode(AppMode m) {
    s_mode = (AppMode)(((int)m % AV_MODE_COUNT + AV_MODE_COUNT) % AV_MODE_COUNT);
    apply_visibility();
}

// Gesture map (PHYSICAL directions; the touch axes report inverted to LVGL —
// verified for vertical, assumed for horizontal, so LV_DIR_* below are the
// opposite of the physical swipe):
//   swipe right -> next mode (wraps)     swipe left -> previous mode
//   swipe down  -> display off           swipe up   -> nothing
//   tap         -> nothing (touch still wakes a sleeping display)
static void gesture_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (!s_display_on) return;   // asleep: pressed_cb handles the wake

    if (dir == LV_DIR_LEFT) {          // physical swipe RIGHT
        lv_indev_wait_release(indev);
        ui_set_mode((AppMode)(s_mode + 1));
    } else if (dir == LV_DIR_RIGHT) {  // physical swipe LEFT
        lv_indev_wait_release(indev);
        ui_set_mode((AppMode)(s_mode - 1));
    } else if (dir == LV_DIR_TOP) {    // physical swipe DOWN
        lv_indev_wait_release(indev);
        display_set(false);
    }
    // LV_DIR_BOTTOM (physical swipe UP): reserved, does nothing.
}

// Fires on finger-down anywhere: if the display is asleep, wake it and
// swallow the press so it doesn't also switch modes.
static void pressed_cb(lv_event_t *e) {
    (void)e;
    if (!s_display_on) {
        display_set(true);
        lv_indev_wait_release(lv_indev_get_act());
    }
}

void ui_init(lv_indev_t *indev) {
    (void)indev;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Waiting-for-host banner (idle only).
    s_usb_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_usb_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_usb_label, lv_color_white(), 0);
    lv_label_set_text(s_usb_label, "1U Visualizer — waiting for USB audio...");
    lv_obj_align(s_usb_label, LV_ALIGN_CENTER, 0, 0);

    // Boot/diagnostic status lines (idle only).
    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(120, 120, 120), 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_status_label, AV_DISP_W - 8);
    lv_label_set_text(s_status_label, "boot");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 4, 4);


    // Visualizer modes (hidden until streaming).
    s_mode_obj[AV_MODE_PRODUCER] = mode_producer_create(scr);
    s_mode_obj[AV_MODE_VIBE]     = mode_vibe_create(scr);
    s_mode_obj[AV_MODE_LUFS]     = mode_lufs_create(scr);
    s_mode_obj[AV_MODE_TUNNEL]   = mode_tunnel_create(scr);
    s_mode_obj[AV_MODE_CLOCK]    = mode_clock_create(scr);

    // Touch: see gesture_cb for the swipe map. Handlers go on the screen AND
    // each full-screen mode panel (the visible panel receives the input).
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, pressed_cb, LV_EVENT_PRESSED, NULL);
    for (int i = 0; i < AV_MODE_COUNT; i++) {
        lv_obj_add_event_cb(s_mode_obj[i], gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_mode_obj[i], pressed_cb, LV_EVENT_PRESSED, NULL);
    }

    s_last_active_ms = lv_tick_get();
    apply_visibility();
}

void ui_status(const char *msg) {
    if (!s_status_label || s_streaming || s_mode == AV_MODE_CLOCK) return;
    lv_label_set_text(s_status_label, msg);
    lv_refr_now(NULL);   // force synchronous flush so it shows even if the next step blocks
}


void ui_update(void) {
    bool streaming = usb_audio_streaming();
    if (streaming != s_streaming) {
        s_streaming = streaming;
        apply_visibility();
    }

    // Display sleep management. A NEW stream wakes the panel (edge-triggered,
    // so a manual swipe-down sleep sticks even while audio keeps playing);
    // while awake, streaming holds off the idle timer, and without streaming
    // the panel dozes off after the timeout.
    if (streaming && !s_was_streaming) display_set(true);
    s_was_streaming = streaming;
    if (s_display_on) {
        // The clock view is a desk clock: it never auto-sleeps.
        if (streaming || s_mode == AV_MODE_CLOCK) s_last_active_ms = lv_tick_get();
        else if (lv_tick_elaps(s_last_active_ms) > AV_DISPLAY_TIMEOUT_MS) {
            display_set(false);
        }
    }

    if (!s_display_on) return;
    if (!s_streaming && s_mode != AV_MODE_CLOCK) return;

    VisualizerState vs;
    vis_acquire(&vs);
    switch (s_mode) {
        case AV_MODE_PRODUCER: mode_producer_update(&vs); break;
        case AV_MODE_VIBE:     mode_vibe_update(&vs);     break;
        case AV_MODE_LUFS:     mode_lufs_update(&vs);     break;
        case AV_MODE_TUNNEL:   mode_tunnel_update(&vs);   break;
        case AV_MODE_CLOCK:    mode_clock_update(&vs);    break;
        default: break;
    }
}
