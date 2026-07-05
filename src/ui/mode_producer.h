#ifndef AV_MODE_PRODUCER_H
#define AV_MODE_PRODUCER_H

#include "lvgl.h"
#include "dsp/vis_state.h"

// Builds the Producer view (31-band FFT + stereo peak/clip meters + phase bar)
// as a full-screen child of `parent`. Returns the root container.
lv_obj_t *mode_producer_create(lv_obj_t *parent);

// Updates the widgets from the latest analyzer snapshot. Call only when this
// mode is visible.
void mode_producer_update(const VisualizerState *vs);

#endif // AV_MODE_PRODUCER_H
