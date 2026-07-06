#ifndef AV_MODE_STARFIELD_H
#define AV_MODE_STARFIELD_H

#include "lvgl.h"
#include "dsp/vis_state.h"

lv_obj_t *mode_starfield_create(lv_obj_t *parent);
void mode_starfield_update(const VisualizerState *vs);

#endif // AV_MODE_STARFIELD_H
