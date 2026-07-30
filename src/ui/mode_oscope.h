#ifndef AV_MODE_OSCOPE_H
#define AV_MODE_OSCOPE_H

#include "lvgl.h"
#include "dsp/vis_state.h"

// Builds the Oscilloscope view (dual-trace stereo waveform over a graticule)
// as a full-screen child of `parent`. Returns the root container.
lv_obj_t *mode_oscope_create(lv_obj_t *parent);

// Updates the trace from the latest analyzer snapshot. Call only when this
// mode is visible.
void mode_oscope_update(const VisualizerState *vs);

#endif // AV_MODE_OSCOPE_H
