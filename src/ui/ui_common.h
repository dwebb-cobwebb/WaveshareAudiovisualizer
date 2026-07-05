#ifndef AV_UI_COMMON_H
#define AV_UI_COMMON_H

#include "lvgl.h"
#include "app.h"

// Builds both mode views and wires gesture/tap mode switching.
void ui_init(lv_indev_t *indev);

// Acquires the latest analyzer snapshot and updates the visible mode.
// Call from the core0 loop (e.g. every ~10-20 ms).
void ui_update(void);

// Idle status line shown at the top-left of the screen (hidden while
// streaming). Sets the text and forces an immediate synchronous refresh so it
// is visible even if the next init step blocks.
void ui_status(const char *msg);

AppMode ui_current_mode(void);
void    ui_set_mode(AppMode m);

#endif // AV_UI_COMMON_H
