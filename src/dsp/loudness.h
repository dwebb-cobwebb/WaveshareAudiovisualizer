#ifndef AV_LOUDNESS_H
#define AV_LOUDNESS_H

#include <stdint.h>

// EBU R128 / ITU-R BS.1770-4 loudness measurement (stereo, 48 kHz).
// Runs entirely on the analyzer core; snapshots are published to the UI via
// VisualizerState.

typedef struct {
    float lufs_m;    // momentary (400 ms), LUFS; <= -120 when silent
    float lufs_s;    // short-term (3 s), LUFS
    float lufs_i;    // integrated (gated), LUFS
    float lra;       // loudness range, LU (EBU Tech 3342)
    float tp_db;     // max true peak since reset, dBTP (max of L/R)
} loudness_snapshot_t;

void loudness_init(void);

// Feed interleaved stereo float frames (every sample exactly once).
void loudness_process(const float *interleaved, uint32_t frames);

// Copy the latest snapshot (updated every 100 ms block).
void loudness_get(loudness_snapshot_t *out);

// Request a reset of integrated loudness, LRA and true-peak hold.
// Safe to call from the other core; applied at the next processed block.
void loudness_request_reset(void);

#endif // AV_LOUDNESS_H
