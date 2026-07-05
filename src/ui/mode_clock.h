#ifndef AV_MODE_CLOCK_H
#define AV_MODE_CLOCK_H

#include "lvgl.h"
#include "dsp/vis_state.h"

lv_obj_t *mode_clock_create(lv_obj_t *parent);
void mode_clock_update(const VisualizerState *vs);

#endif // AV_MODE_CLOCK_H
