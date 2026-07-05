#include "display/lv_port_disp.h"
#include "display/axs15231b.h"
#include "config.h"

#include "lvgl.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// Full-screen buffer matching the Waveshare demo (full_refresh=1).
// The 0x32 RAMWR (quad write) always starts at y=0 regardless of RASET;
// full_refresh ensures the flush always covers the whole panel from y=0.
static lv_color_t s_buf[AV_DISP_NATIVE_W * AV_DISP_NATIVE_H];  // 172*640*2 = 220KB
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    // MADCTL MV=1 requires the LVGL landscape buffer to be transposed before
    // writing to the portrait-addressed GRAM. axs_blit_pio_landscape handles both.
    axs_blit_pio_landscape((const uint16_t *)px);
    lv_disp_flush_ready(drv);
}

static bool tick_timer_cb(repeating_timer_t *t) {
    (void)t;
    lv_tick_inc(1);
    return true;
}
static repeating_timer_t s_tick_timer;

void lv_port_disp_init(void) {
    axs_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, AV_DISP_NATIVE_W * AV_DISP_NATIVE_H);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf    = &s_draw_buf;
    s_disp_drv.flush_cb    = disp_flush_cb;
    s_disp_drv.hor_res     = AV_DISP_W;   // 640 — landscape logical width
    s_disp_drv.ver_res     = AV_DISP_H;   // 172 — landscape logical height
    s_disp_drv.full_refresh = 1;
    lv_disp_drv_register(&s_disp_drv);

    add_repeating_timer_ms(1, tick_timer_cb, NULL, &s_tick_timer);
}
