#ifndef AV_TOUCH_AXS_H
#define AV_TOUCH_AXS_H

#include <stdint.h>
#include <stdbool.h>

void touch_init(void);

// Reads the current touch point in landscape coordinates.
// Returns true if a finger is down; fills *x,*y (0..AV_DISP_W/H-1).
bool touch_read(uint16_t *x, uint16_t *y);

#endif // AV_TOUCH_AXS_H
