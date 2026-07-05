#ifndef AV_ANALYZER_H
#define AV_ANALYZER_H

#include "dsp/ringbuffer.h"
#include "dsp/vis_state.h"

// Initializes FFT instance, Hann window, and log-spaced band edges.
void analyzer_init(void);

// Runs on the DSP core. Consumes one hop from the ring, computes a full
// VisualizerState, and publishes it. Returns true if a frame was produced
// (i.e. enough samples were available), false otherwise.
bool analyzer_process(AudioRing *ring);

// Entry point for core1 (passed to multicore_launch_core1). Loops forever:
// pulls from the shared ring and publishes states.
void analyzer_core1_main(void);

#endif // AV_ANALYZER_H
