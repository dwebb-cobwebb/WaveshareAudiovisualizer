#ifndef AV_MODE_LUFS_H
#define AV_MODE_LUFS_H

#include "lvgl.h"
#include "dsp/vis_state.h"

lv_obj_t *mode_lufs_create(lv_obj_t *parent);
void mode_lufs_update(const VisualizerState *vs);

#endif // AV_MODE_LUFS_H
