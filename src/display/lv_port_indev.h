#ifndef AV_LV_PORT_INDEV_H
#define AV_LV_PORT_INDEV_H

#include "lvgl.h"

// Registers the capacitive touch as an LVGL pointer input device.
// Returns the created indev (used for gesture detection in ui_common).
lv_indev_t *lv_port_indev_init(void);

#endif // AV_LV_PORT_INDEV_H
