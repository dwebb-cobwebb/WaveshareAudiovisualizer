#ifndef AV_MODE_GONIOMETER_H
#define AV_MODE_GONIOMETER_H

#include "lvgl.h"
#include "dsp/vis_state.h"

// Builds the Goniometer view (stereo M/S scatter plot, density-weighted
// phosphor trail) as a full-screen child of `parent`. Returns the root
// container.
lv_obj_t *mode_goniometer_create(lv_obj_t *parent);

// Updates the trace from the latest analyzer snapshot. Call only when this
// mode is visible.
void mode_goniometer_update(const VisualizerState *vs);

#endif // AV_MODE_GONIOMETER_H
