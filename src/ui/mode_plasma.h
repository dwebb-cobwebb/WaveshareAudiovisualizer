#ifndef AV_MODE_PLASMA_H
#define AV_MODE_PLASMA_H

#include "lvgl.h"
#include "dsp/vis_state.h"

lv_obj_t *mode_plasma_create(lv_obj_t *parent);
void mode_plasma_update(const VisualizerState *vs);

#endif // AV_MODE_PLASMA_H
