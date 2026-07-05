#include "usb/usb_audio.h"
#include "usb/usb_descriptors.h"
#include "app.h"
#include "config.h"

#include "tusb.h"
#include "device/usbd_pvt.h"   // usbd_class_driver_t, usbd_edpt_* (custom class)
#include "hardware/structs/usb.h"        // usb_hw (E12 status-poll workaround)
#include "hardware/structs/usb_dpram.h"  // direct endpoint buffer drive
#include "hardware/address_mapped.h"     // hw_clear_alias
#include "hardware/irq.h"
#include "pico/time.h"
#if __has_include("bsp/board_api.h")
#  include "bsp/board_api.h"
#else
#  include "bsp/board.h"
#endif

#include <string.h>

// ===========================================================================
// Hand-rolled UAC 1.0 class driver.
//
// TinyUSB's built-in audio class only speaks UAC2. We need UAC1 so Windows
// binds usbaudio.sys: usbaudio2.sys issues GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS
// for full-speed devices, which some host stacks answer NOT_SUPPORTED — the
// stream then never starts. This driver registers with the device stack via
// usbd_app_driver_get_cb() and implements just enough of UAC1 for a stereo
// 48 kHz / 16-bit sink: the streaming interface's alt-setting switch, the
// feature-unit mute/volume controls, the endpoint sampling-frequency control,
// and the isochronous OUT data path.
// ===========================================================================

// ---------------------------------------------------------------------------
// UAC1 control request codes (bRequest). The 0x80 bit selects GET vs SET.
// ---------------------------------------------------------------------------
enum {
    UAC1_REQ_SET_CUR = 0x01,
    UAC1_REQ_GET_CUR = 0x81,
    UAC1_REQ_GET_MIN = 0x82,
    UAC1_REQ_GET_MAX = 0x83,
    UAC1_REQ_GET_RES = 0x84,
};

// Control selectors (high byte of wValue)
enum {
    UAC1_MUTE_CONTROL          = 0x01,
    UAC1_VOLUME_CONTROL        = 0x02,
    UAC1_SAMPLING_FREQ_CONTROL = 0x01,   // endpoint control
};

// Volume range, 1/256 dB (S16). 0 dB .. -60 dB, 1 dB step.
#define VOL_MIN   ((int16_t)0xC400)   // -60 dB
#define VOL_MAX   0x0000
#define VOL_RES   0x0100              //  +1 dB

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static volatile bool s_streaming = false;
static uint8_t  s_alt = 0;
static uint32_t s_sample_rate = AV_SAMPLE_RATE_HZ;
static int8_t   s_mute   = 0;
static int16_t  s_volume[AV_CHANNELS + 1] = { 0 };   // index 0 = master, 1..N per channel

// Endpoint descriptor for the iso OUT EP, filled at open() for iso alloc/activate.
static tusb_desc_endpoint_t s_ep_out;

// Scratch for PCM->float conversion.
#define MAX_FRAMES_PER_PKT  (48 + 1)
static float s_pkt_f32[MAX_FRAMES_PER_PKT * AV_CHANNELS];

// Control-transfer scratch (holds a SET value while its DATA stage completes).
static uint8_t s_ctrl_buf[4];

// Receive counters for the idle status line.
static volatile uint32_t s_rx_pkts  = 0;
static volatile uint32_t s_rx_bytes = 0;

static bool iso_poll_timer_cb(repeating_timer_t *t);
static repeating_timer_t s_iso_poll_timer;
static void iso_poll_direct(void);

// ---------------------------------------------------------------------------
// Public interface (called from main.c on core0)
// ---------------------------------------------------------------------------
void usb_audio_init(void) {
    board_init();
    tusb_init();
    // -500us: fixed 2 kHz cadence (twice the packet rate) regardless of how
    // long the callback takes.
    add_repeating_timer_us(-500, iso_poll_timer_cb, NULL, &s_iso_poll_timer);
}

// Iso packets land every 1 ms into a single hardware buffer, but the main
// loop can be busy for ~12 ms in the display blit. Poll from a hardware
// repeating timer (IRQ context) so reception never depends on the loop.
static bool iso_poll_timer_cb(repeating_timer_t *t) {
    (void)t;
    iso_poll_direct();
    return true;
}

void usb_audio_task(void) {
    // RP2350-E12 ("inadequate synchronisation of USB status signals", only
    // mitigated in later steppings) can leave a completion latched in
    // buf_status without the IRQ ever firing. EP1-OUT (bit 3) is consumed by
    // the poll timer; re-pend the IRQ for anything else (EP0 control).
    if (usb_hw->buf_status & ~(1u << 3)) {
        irq_set_pending(USBCTRL_IRQ);
    }
    tud_task();
}

bool usb_audio_streaming(void) {
    return s_streaming;
}

void usb_audio_debug(usb_audio_dbg_t *out) {
    out->rx_pkts  = s_rx_pkts;
    out->rx_bytes = s_rx_bytes;
}

// ---------------------------------------------------------------------------
// Iso OUT data path — DIRECT HARDWARE DRIVE.
//
// The DCD/TinyUSB transfer path delivers zero completions on this silicon
// despite a provably correct endpoint setup (RP2350-E12 family: USB status
// signals lost between SIE and CPU). The endpoint buffer is therefore driven
// straight from application code: arm the dpram buffer control ourselves and
// poll for FULL — no usbd_edpt_xfer, no interrupt dependency.
// ---------------------------------------------------------------------------
#define BUFC_LEN_MASK   0x03FFu
#define BUFC_AVAIL      (1u << 10)
#define BUFC_STALL      (1u << 11)
#define BUFC_RESET_SEL  (1u << 12)   // reset buffer select to buffer 0
#define BUFC_PID_DATA1  (1u << 13)
#define BUFC_LAST       (1u << 14)
#define BUFC_FULL       (1u << 15)

// EP1 OUT register slots in DPRAM.
#define EP1_OUT_CTRL    (usb_dpram->ep_ctrl[0].out)
#define EP1_OUT_BUFC    (usb_dpram->ep_buf_ctrl[1].out)

// Arm buffer 0 for one 192-byte DATA0 iso packet (full-speed iso is always
// DATA0). Respects the RP2040/RP2350 concurrent-access rule: write everything
// except AVAILABLE first, delay >=12 sys-clk cycles, then set AVAILABLE.
static void iso_arm_direct(void) {
    uint32_t v = AUDIO_OUT_EP_SIZE | BUFC_LAST | BUFC_RESET_SEL;   // PID=DATA0
    EP1_OUT_BUFC = v;
    busy_wait_at_least_cycles(12);
    EP1_OUT_BUFC = v | BUFC_AVAIL;
}

// Poll for a landed packet; convert to float, push to the cross-core ring,
// re-arm. Runs in timer-IRQ context.
static void iso_poll_direct(void) {
    if (!s_streaming) return;
    uint32_t v = EP1_OUT_BUFC;
    if (!(v & BUFC_FULL)) return;

    uint32_t n = v & BUFC_LEN_MASK;
    s_rx_pkts++;
    s_rx_bytes = n;

    // Data buffer address comes from the EP control register's dpram offset.
    const uint8_t *buf = (const uint8_t *)usb_dpram + (EP1_OUT_CTRL & 0xFFFFu);

    uint32_t frames = (n / sizeof(int16_t)) / AV_CHANNELS;
    if (frames > MAX_FRAMES_PER_PKT) frames = MAX_FRAMES_PER_PKT;
    const int16_t *pcm = (const int16_t *)buf;
    const float inv = 1.0f / 32768.0f;
    for (uint32_t i = 0; i < frames * AV_CHANNELS; i++) {
        s_pkt_f32[i] = (float)pcm[i] * inv;
    }
    ring_write(&g_audio_ring, s_pkt_f32, frames);

    // Clear any latched buff-status for EP1-OUT (bit 3) so the DCD's IRQ
    // handler never tries to process a transfer it didn't start.
    hw_clear_alias(usb_hw)->buf_status = (1u << 3);

    iso_arm_direct();
}

// ---------------------------------------------------------------------------
// Class driver callbacks
// ---------------------------------------------------------------------------
static void ac_init(void) {
    s_streaming = false;
    s_alt = 0;
}

static bool ac_deinit(void) {
    return true;
}

static void ac_reset(uint8_t rhport) {
    (void)rhport;
    s_streaming = false;
    s_alt = 0;
}

// open() is called for our first (AudioControl) interface. Because the config
// has an IAD with bInterfaceCount=2, the stack binds both audio interfaces to
// us. We allocate the iso OUT endpoint buffer here (activated later on alt=1)
// and claim the whole audio function's descriptor length.
static uint16_t ac_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf,
                        uint16_t max_len) {
    // Only claim the AudioControl interface of our function.
    if (!(desc_itf->bInterfaceClass == TUSB_CLASS_AUDIO &&
          desc_itf->bInterfaceSubClass == 0x01 /*AudioControl*/ &&
          desc_itf->bInterfaceNumber == ITF_NUM_AUDIO_CONTROL)) {
        return 0;
    }
    TU_VERIFY(max_len >= AUDIO_FUNC_DESC_LEN, 0);

    // Build the iso OUT endpoint descriptor and pre-allocate its packet buffer.
    s_ep_out.bLength          = sizeof(tusb_desc_endpoint_t);
    s_ep_out.bDescriptorType  = TUSB_DESC_ENDPOINT;
    s_ep_out.bEndpointAddress = EPNUM_AUDIO_OUT;
    s_ep_out.bmAttributes.xfer  = TUSB_XFER_ISOCHRONOUS;
    s_ep_out.bmAttributes.sync  = 2; // adaptive
    s_ep_out.bmAttributes.usage = 0; // data
    s_ep_out.wMaxPacketSize   = AUDIO_OUT_EP_SIZE;
    s_ep_out.bInterval        = 1;

    usbd_edpt_iso_alloc(rhport, EPNUM_AUDIO_OUT, AUDIO_OUT_EP_SIZE);

    return AUDIO_FUNC_DESC_LEN;
}

// Feature-unit (interface) control requests: mute + volume.
static bool feature_unit_control(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const *req) {
    uint8_t const selector = TU_U16_HIGH(req->wValue);
    uint8_t const channel  = TU_U16_LOW(req->wValue);

    if (selector == UAC1_MUTE_CONTROL) {
        if (req->bRequest == UAC1_REQ_GET_CUR) {
            if (stage == CONTROL_STAGE_SETUP) {
                s_ctrl_buf[0] = (uint8_t)s_mute;
                return tud_control_xfer(rhport, req, s_ctrl_buf, 1);
            }
            return true;
        }
        if (req->bRequest == UAC1_REQ_SET_CUR) {
            if (stage == CONTROL_STAGE_SETUP) {
                return tud_control_xfer(rhport, req, s_ctrl_buf, 1);
            }
            if (stage == CONTROL_STAGE_ACK) {
                s_mute = (int8_t)s_ctrl_buf[0];
            }
            return true;
        }
    } else if (selector == UAC1_VOLUME_CONTROL) {
        uint8_t ch = (channel <= AV_CHANNELS) ? channel : 0;
        int16_t val;
        switch (req->bRequest) {
            case UAC1_REQ_GET_CUR: val = s_volume[ch]; break;
            case UAC1_REQ_GET_MIN: val = VOL_MIN; break;
            case UAC1_REQ_GET_MAX: val = VOL_MAX; break;
            case UAC1_REQ_GET_RES: val = VOL_RES; break;
            case UAC1_REQ_SET_CUR:
                if (stage == CONTROL_STAGE_SETUP) {
                    return tud_control_xfer(rhport, req, s_ctrl_buf, 2);
                }
                if (stage == CONTROL_STAGE_ACK) {
                    int16_t v = (int16_t)((uint16_t)s_ctrl_buf[0] |
                                          ((uint16_t)s_ctrl_buf[1] << 8));
                    s_volume[ch] = v;
                }
                return true;
            default:
                return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            memcpy(s_ctrl_buf, &val, 2);
            return tud_control_xfer(rhport, req, s_ctrl_buf, 2);
        }
        return true;
    }
    return false;
}

// Endpoint control request: sampling frequency (3-byte little-endian).
static bool endpoint_control(uint8_t rhport, uint8_t stage,
                             tusb_control_request_t const *req) {
    if (TU_U16_HIGH(req->wValue) != UAC1_SAMPLING_FREQ_CONTROL) return false;

    if (req->bRequest == UAC1_REQ_GET_CUR) {
        if (stage == CONTROL_STAGE_SETUP) {
            s_ctrl_buf[0] = (uint8_t)(s_sample_rate & 0xFF);
            s_ctrl_buf[1] = (uint8_t)((s_sample_rate >> 8) & 0xFF);
            s_ctrl_buf[2] = (uint8_t)((s_sample_rate >> 16) & 0xFF);
            return tud_control_xfer(rhport, req, s_ctrl_buf, 3);
        }
        return true;
    }
    if (req->bRequest == UAC1_REQ_SET_CUR) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, req, s_ctrl_buf, 3);
        }
        if (stage == CONTROL_STAGE_ACK) {
            s_sample_rate = (uint32_t)s_ctrl_buf[0] |
                            ((uint32_t)s_ctrl_buf[1] << 8) |
                            ((uint32_t)s_ctrl_buf[2] << 16);
        }
        return true;
    }
    return false;
}

static bool ac_control_xfer(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *request) {
    uint8_t const rcpt = request->bmRequestType_bit.recipient;
    uint8_t const type = request->bmRequestType_bit.type;

    // ---- Standard interface requests: SET/GET_INTERFACE (alt-setting) ----
    if (type == TUSB_REQ_TYPE_STANDARD && rcpt == TUSB_REQ_RCPT_INTERFACE) {
        uint8_t const itf = TU_U16_LOW(request->wIndex);
        if (itf != ITF_NUM_AUDIO_STREAMING) return false;

        if (request->bRequest == TUSB_REQ_SET_INTERFACE) {
            if (stage != CONTROL_STAGE_SETUP) return true;
            uint8_t const alt = (uint8_t)request->wValue;
            s_alt = alt;

            if (alt == 1) {
                usbd_edpt_iso_activate(rhport, &s_ep_out);
                // Keep the DCD's per-buffer interrupt away from this EP —
                // completions are consumed by polling (iso_poll_direct), and
                // an unexpected IRQ would make the DCD process a transfer it
                // never started.
                EP1_OUT_CTRL &= ~(1u << 29);   // EP_CTRL_INTERRUPT_PER_BUFFER
                s_streaming = true;
                iso_arm_direct();
            } else {
                usbd_edpt_close(rhport, EPNUM_AUDIO_OUT);
                s_streaming = false;
            }
            return tud_control_status(rhport, request);
        }
        if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
            if (stage != CONTROL_STAGE_SETUP) return true;
            return tud_control_xfer(rhport, request, &s_alt, 1);
        }
        return false;
    }

    // ---- Class-specific requests: feature unit (itf) / sample freq (ep) ----
    if (type == TUSB_REQ_TYPE_CLASS) {
        if (rcpt == TUSB_REQ_RCPT_INTERFACE) {
            return feature_unit_control(rhport, stage, request);
        }
        if (rcpt == TUSB_REQ_RCPT_ENDPOINT) {
            return endpoint_control(rhport, stage, request);
        }
    }
    return false;
}

static bool ac_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                       uint32_t xferred_bytes) {
    // Iso OUT is consumed by direct polling (iso_poll_direct); nothing routes
    // here in normal operation.
    (void)rhport; (void)ep_addr; (void)result; (void)xferred_bytes;
    return true;
}

static const usbd_class_driver_t s_uac1_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "UAC1",
#endif
    .init           = ac_init,
    .deinit         = ac_deinit,
    .reset          = ac_reset,
    .open           = ac_open,
    .control_xfer_cb = ac_control_xfer,
    .xfer_cb        = ac_xfer_cb,
    .sof            = NULL,
};

// Registered by the device stack in addition to the built-in classes.
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_uac1_driver;
}
