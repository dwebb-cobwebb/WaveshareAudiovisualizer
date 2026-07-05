#include "dsp/ringbuffer.h"

#define RING_MASK (AV_RING_FRAMES - 1)
_Static_assert((AV_RING_FRAMES & (AV_RING_FRAMES - 1)) == 0,
               "AV_RING_FRAMES must be a power of two");

void ring_init(AudioRing *r) {
    r->head = 0;
    r->tail = 0;
}

uint32_t ring_available(const AudioRing *r) {
    return r->head - r->tail;  // unsigned wrap-safe
}

uint32_t ring_write(AudioRing *r, const float *interleaved, uint32_t frames) {
    uint32_t free_frames = AV_RING_FRAMES - (r->head - r->tail);
    if (frames > free_frames) frames = free_frames;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (r->head + i) & RING_MASK;
        r->buf[idx * AV_CHANNELS + 0] = interleaved[i * AV_CHANNELS + 0];
        r->buf[idx * AV_CHANNELS + 1] = interleaved[i * AV_CHANNELS + 1];
    }
    __asm volatile("" ::: "memory");
    r->head += frames;
    return frames;
}

uint32_t ring_peek(const AudioRing *r, float *out, uint32_t frames) {
    uint32_t avail = r->head - r->tail;
    if (frames > avail) frames = avail;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (r->tail + i) & RING_MASK;
        out[i * AV_CHANNELS + 0] = r->buf[idx * AV_CHANNELS + 0];
        out[i * AV_CHANNELS + 1] = r->buf[idx * AV_CHANNELS + 1];
    }
    return frames;
}

void ring_advance(AudioRing *r, uint32_t frames) {
    uint32_t avail = r->head - r->tail;
    if (frames > avail) frames = avail;
    r->tail += frames;
}
