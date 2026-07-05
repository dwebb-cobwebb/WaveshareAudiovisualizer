#include "display/lv_port_indev.h"
#include "display/touch_axs.h"

#include "lvgl.h"

static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_indev_drv_t s_indev_drv;

lv_indev_t *lv_port_indev_init(void) {
    touch_init();
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read_cb;
    return lv_indev_drv_register(&s_indev_drv);
}
