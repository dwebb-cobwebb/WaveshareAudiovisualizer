#ifndef AV_USB_AUDIO_H
#define AV_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Initializes TinyUSB device stack. Call once from core0 after board init.
void usb_audio_init(void);

// Pump the TinyUSB device task. Call frequently from the core0 loop.
void usb_audio_task(void);

// True once the host has activated the audio streaming interface.
bool usb_audio_streaming(void);

// Diagnostic snapshot of the stream-start path (for on-screen tracing).
typedef struct {
    uint32_t setitf_count;   // # of SET_INTERFACE on the streaming interface
    uint8_t  last_alt;       // last alt-setting requested (0xFF = none yet)
    uint32_t rx_pkts;        // # of audio OUT packets received
    uint32_t rx_bytes;       // bytes in the most recent OUT packet
    uint32_t get_count;      // # of class GET entity requests
    uint32_t set_count;      // # of class SET entity requests
    uint32_t close_count;    // # of close-EP (alt 0) callbacks
    uint32_t last_req;       // last control req: entity<<16 | selector<<8 | bRequest
    uint8_t  open_diag;      // 0x10 = open() claimed, 0x20 = iso_alloc ok
    uint32_t alt1_count;     // # of SET_INTERFACE(alt=1) received
    uint32_t alt_hist;       // last 4 alts, one byte each: 0xA0|alt (00 = none)
    uint32_t inv_count;      // # of control_xfer_cb SETUP invocations (any req)
    uint32_t last_std;       // last std-itf request: bmReqType<<24|bRequest<<16|wValue
} usb_audio_dbg_t;

void usb_audio_debug(usb_audio_dbg_t *out);

#endif // AV_USB_AUDIO_H
