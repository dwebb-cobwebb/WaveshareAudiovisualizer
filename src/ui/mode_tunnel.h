#ifndef AV_MODE_TUNNEL_H
#define AV_MODE_TUNNEL_H

#include "lvgl.h"
#include "dsp/vis_state.h"

lv_obj_t *mode_tunnel_create(lv_obj_t *parent);
void mode_tunnel_update(const VisualizerState *vs);

#endif // AV_MODE_TUNNEL_H
