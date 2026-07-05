#ifndef AV_APP_H
#define AV_APP_H

#include "dsp/ringbuffer.h"

// Cross-core audio ring: producer = USB host-OUT callback (core0/USB),
// consumer = analyzer (core1). Defined in main.c.
extern AudioRing g_audio_ring;

typedef enum {
    AV_MODE_PRODUCER = 0,   // Mode A: 31-band FFT + peak/clip + phase bar
    AV_MODE_VIBE     = 1,   // Mode B: VU needles + neon decay FFT
    AV_MODE_COUNT
} AppMode;

#endif // AV_APP_H
