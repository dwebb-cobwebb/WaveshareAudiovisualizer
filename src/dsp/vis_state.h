#ifndef AV_VIS_STATE_H
#define AV_VIS_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Snapshot of everything the renderer needs for one frame. Produced by the
// DSP core, consumed by the UI core via a double-buffer (see vis_publish/
// vis_acquire). Keep this POD and small — it is copied across cores.
typedef struct {
    float bands[AV_NUM_BANDS];   // 0..1 normalized, smoothed band magnitudes
    float band_peak[AV_NUM_BANDS]; // 0..1 peak-hold per band

    float rms_l;                 // 0..1 RMS level, left
    float rms_r;                 // 0..1 RMS level, right
    float peak_l;                // 0..1 instantaneous peak, left
    float peak_r;                // 0..1 instantaneous peak, right

    float correlation;           // -1..+1 stereo phase correlation

    bool  clip_l;                // clip-hold latched (>= 0 dBFS within hold window)
    bool  clip_r;

    uint32_t frame_id;           // increments each published frame
} VisualizerState;

// Double-buffered publish/acquire. Implemented in analyzer.c.
// DSP core calls vis_publish() with its freshly computed state.
// UI core calls vis_acquire() to copy the latest stable snapshot.
void vis_state_init(void);
void vis_publish(const VisualizerState *s);
void vis_acquire(VisualizerState *out);

#endif // AV_VIS_STATE_H
