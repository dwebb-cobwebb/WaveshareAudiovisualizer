#ifndef AV_MODE_VIBE_H
#define AV_MODE_VIBE_H

#include "lvgl.h"
#include "dsp/vis_state.h"

// Builds the Vibe Deck view (two analog VU needles + neon decay FFT bars)
// as a full-screen child of `parent`. Returns the root container.
lv_obj_t *mode_vibe_create(lv_obj_t *parent);

// Updates widgets from the latest snapshot. Call only when this mode is visible.
void mode_vibe_update(const VisualizerState *vs);

#endif // AV_MODE_VIBE_H
