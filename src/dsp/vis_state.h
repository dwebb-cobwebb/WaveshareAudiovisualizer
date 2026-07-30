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

    // EBU R128 loudness (see dsp/loudness.h). <= -120 means silence/unset.
    float lufs_m;                // momentary (400 ms), LUFS
    float lufs_s;                // short-term (3 s), LUFS
    float lufs_i;                // integrated (gated), LUFS
    float lra;                   // loudness range, LU
    float tp_db;                 // max true peak since reset, dBTP

    bool  clip_l;                // clip-hold latched (>= 0 dBFS within hold window)
    bool  clip_r;

    // Oscilloscope trace: min/max sample amplitude per column over the most
    // recent analysis window (see analyzer.c), quantized to int8
    // (-127..127 == -1.0..+1.0 FS). One column per landscape pixel column.
    int8_t scope_min_l[AV_SCOPE_COLS];
    int8_t scope_max_l[AV_SCOPE_COLS];
    int8_t scope_min_r[AV_SCOPE_COLS];
    int8_t scope_max_r[AV_SCOPE_COLS];

    // Goniometer: raw (untriggered) sample pairs spread evenly across the
    // analysis window, quantized to int8 (-127..127 == -1.0..+1.0 FS).
    int8_t gonio_l[AV_GONIO_POINTS];
    int8_t gonio_r[AV_GONIO_POINTS];

    uint32_t frame_id;           // increments each published frame
} VisualizerState;

// Double-buffered publish/acquire. Implemented in analyzer.c.
// DSP core calls vis_publish() with its freshly computed state.
// UI core calls vis_acquire() to copy the latest stable snapshot.
void vis_state_init(void);
void vis_publish(const VisualizerState *s);
void vis_acquire(VisualizerState *out);

// Lightweight accessor for callers that only need the frame counter (e.g. the
// once-a-second heartbeat) — avoids a full VisualizerState-sized static copy
// just to read one field.
uint32_t vis_frame_id(void);

#endif // AV_VIS_STATE_H
