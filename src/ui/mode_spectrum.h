#ifndef AV_MODE_SPECTRUM_H
#define AV_MODE_SPECTRUM_H

#include "lvgl.h"
#include "dsp/vis_state.h"

// Builds the Spectrum view (smooth filled log-frequency curve with a
// peak-hold overlay, FabFilter Pro-Q style) as a full-screen child of
// `parent`. Returns the root container.
lv_obj_t *mode_spectrum_create(lv_obj_t *parent);

// Renders the curve from the latest analyzer snapshot. Call only when this
// mode is visible.
void mode_spectrum_update(const VisualizerState *vs);

#endif // AV_MODE_SPECTRUM_H
