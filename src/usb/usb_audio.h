#ifndef AV_USB_AUDIO_H
#define AV_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Initializes TinyUSB device stack and the iso RX poll timer.
// Call once from core0 after display init.
void usb_audio_init(void);

// Pump the TinyUSB control plane. Call frequently from the core0 loop.
// (Audio data reception runs on its own 500us hardware timer.)
void usb_audio_task(void);

// True once the host has activated the audio streaming interface (alt=1).
bool usb_audio_streaming(void);

// Receive counters for the idle status line.
typedef struct {
    uint32_t rx_pkts;    // # of audio OUT packets received
    uint32_t rx_bytes;   // bytes in the most recent packet
} usb_audio_dbg_t;

void usb_audio_debug(usb_audio_dbg_t *out);

#endif // AV_USB_AUDIO_H
