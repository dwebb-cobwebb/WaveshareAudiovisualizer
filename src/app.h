#ifndef AV_APP_H
#define AV_APP_H

#include "dsp/ringbuffer.h"

// Cross-core audio ring: producer = USB host-OUT callback (core0/USB),
// consumer = analyzer (core1). Defined in main.c.
extern AudioRing g_audio_ring;

typedef enum {
    AV_MODE_PRODUCER = 0,   // Mode A: 31-band FFT + peak/clip + phase bar
    AV_MODE_SPECTRUM = 1,   // Mode B: smooth filled log-frequency curve + peak line
    AV_MODE_VIBE     = 2,   // Mode C: photorealistic stereo VU meters
    AV_MODE_LUFS     = 3,   // Mode D: EBU R128 loudness (M/S/I, LRA, true peak)
    AV_MODE_OSCOPE   = 4,   // Mode E: dual-trace stereo oscilloscope
    AV_MODE_GONIOMETER=5,   // Mode F: stereo M/S goniometer (vectorscope)
    AV_MODE_TUNNEL   = 6,   // Mode G: audio-reactive infinite tunnel (eye candy)
    AV_MODE_STARFIELD= 7,   // Mode H: warp starfield with motion trails
    AV_MODE_PLASMA   = 8,   // Mode I: audio-reactive plasma
    AV_MODE_CLOCK    = 9,   // Mode J: big digital clock (time set via CDC serial)
    AV_MODE_COUNT
} AppMode;

#endif // AV_APP_H
