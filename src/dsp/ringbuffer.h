#ifndef AV_RINGBUFFER_H
#define AV_RINGBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"

// Single-producer (USB core) / single-consumer (DSP core) lock-free ring of
// interleaved stereo float frames. Capacity is AV_RING_FRAMES (power of two).
// Samples are stored normalized to [-1, 1].
typedef struct {
    float    buf[AV_RING_FRAMES * AV_CHANNELS];
    volatile uint32_t head;   // producer writes
    volatile uint32_t tail;   // consumer reads
} AudioRing;

void ring_init(AudioRing *r);

// Producer: push `frames` interleaved stereo frames. Returns frames actually
// written (drops oldest by simply not overwriting unread — caller may overrun).
uint32_t ring_write(AudioRing *r, const float *interleaved, uint32_t frames);

// Number of whole frames available to read.
uint32_t ring_available(const AudioRing *r);

// Consumer: copy `frames` interleaved frames into `out` without advancing the
// read pointer (peek). Returns frames copied. Used to grab an overlapping FFT
// window. Then call ring_advance() to consume the hop.
uint32_t ring_peek(const AudioRing *r, float *out, uint32_t frames);

// Advance read pointer by `frames`.
void ring_advance(AudioRing *r, uint32_t frames);

#endif // AV_RINGBUFFER_H
