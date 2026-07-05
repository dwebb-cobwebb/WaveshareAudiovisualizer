#include "usb_descriptors.h"
#include "tusb.h"
#include "config.h"

// ===========================================================================
// USB descriptors for the "1U Visualizer" — UAC 1.0 stereo speaker (sink).
//
// The host writes a stereo 48 kHz / 16-bit PCM stream to the isochronous OUT
// endpoint; the firmware analyses it. UAC1 (not UAC2) so Windows binds the
// legacy usbaudio.sys, which streams full-speed devices without the
// GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS query that breaks UAC2 here.
// ===========================================================================

#define USB_VID   0xCAFE
#define USB_PID   0x4001
#define USB_BCD   0x0200

// ---------------------------------------------------------------------------
// Device descriptor — composite (IAD) so both audio interfaces group as one
// function. IAD function protocol is 0x00 (UAC1), unlike the UAC2 0x20.
// ---------------------------------------------------------------------------
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// ---------------------------------------------------------------------------
// Configuration descriptor (UAC1, hand-built)
// ---------------------------------------------------------------------------
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 8 /*IAD*/ + AUDIO_FUNC_DESC_LEN)

// UAC1 audio class subtype / selector constants used below
#define AC_HEADER          0x01
#define AC_INPUT_TERMINAL  0x02
#define AC_OUTPUT_TERMINAL 0x03
#define AC_FEATURE_UNIT    0x06
#define AS_GENERAL         0x01
#define AS_FORMAT_TYPE     0x02
#define AS_EP_GENERAL      0x01
#define FORMAT_TYPE_I      0x01

#define AUDIO_SUBCLASS_CONTROL    0x01
#define AUDIO_SUBCLASS_STREAMING  0x02

uint8_t const desc_configuration[] = {
    // Configuration (9)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface Association Descriptor (8) — Audio function, UAC1 (proto 0x00)
    8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_AUDIO_CONTROL, 2,
       TUSB_CLASS_AUDIO, 0x00, 0x00, 0,

    // --- Standard AudioControl interface (9) ---
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_CONTROL, 0, 0,
       TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0x00, 4,

    // --- Class-specific AC interface header (9): bcdADC 0x0100, wTotalLen 40 ---
    9, TUSB_DESC_CS_INTERFACE, AC_HEADER, U16_TO_U8S_LE(0x0100),
       U16_TO_U8S_LE(40), 1, ITF_NUM_AUDIO_STREAMING,

    // --- Input Terminal (12): ID1, USB streaming, 2ch (L+R) ---
    12, TUSB_DESC_CS_INTERFACE, AC_INPUT_TERMINAL, UAC1_ENTITY_INPUT_TERMINAL,
        U16_TO_U8S_LE(0x0101), 0x00, 0x02, U16_TO_U8S_LE(0x0003), 0x00, 0x00,

    // --- Feature Unit (10): ID2 <- IT1; master=mute, L/R=volume ---
    10, TUSB_DESC_CS_INTERFACE, AC_FEATURE_UNIT, UAC1_ENTITY_FEATURE_UNIT,
        UAC1_ENTITY_INPUT_TERMINAL, 0x01 /*bControlSize*/,
        0x01 /*master: mute*/, 0x02 /*L: volume*/, 0x02 /*R: volume*/, 0x00,

    // --- Output Terminal (9): ID3 speaker <- FU2 ---
    9, TUSB_DESC_CS_INTERFACE, AC_OUTPUT_TERMINAL, UAC1_ENTITY_OUTPUT_TERMINAL,
       U16_TO_U8S_LE(0x0301), 0x00, UAC1_ENTITY_FEATURE_UNIT, 0x00,

    // --- Standard AS interface, alt 0 (9) — zero-bandwidth ---
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAMING, 0, 0,
       TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0x00, 0,

    // --- Standard AS interface, alt 1 (9) — one iso OUT endpoint ---
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAMING, 1, 1,
       TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0x00, 0,

    // --- Class-specific AS general (7): links IT1, PCM format ---
    7, TUSB_DESC_CS_INTERFACE, AS_GENERAL, UAC1_ENTITY_INPUT_TERMINAL,
       0x01 /*bDelay*/, U16_TO_U8S_LE(0x0001) /*PCM*/,

    // --- Type I format (11): 2ch, 2 bytes/subframe, 16-bit, 1 rate = 48000 ---
    11, TUSB_DESC_CS_INTERFACE, AS_FORMAT_TYPE, FORMAT_TYPE_I,
        0x02, 0x02, 16, 0x01, 0x80, 0xBB, 0x00,

    // --- Standard iso OUT endpoint (9, UAC1): adaptive data, 196 B, 1 ms ---
    // bmAttributes 0x09 = iso | adaptive | data (enum values are pre-shifted).
    9, TUSB_DESC_ENDPOINT, EPNUM_AUDIO_OUT,
       (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA),
       U16_TO_U8S_LE(AUDIO_OUT_EP_SIZE), 0x01, 0x00 /*bRefresh*/, 0x00 /*bSynchAddr*/,

    // --- Class-specific AS iso data EP (7) ---
    // bmAttributes 0x01 = Sampling Frequency Control PRESENT. Both working
    // reference devices on the test PC (Plantronics UAC1 FS adaptive, Zoom
    // U-44) declare this; usbaudio.sys programs the rate via this endpoint
    // control and may never commit alt=1 if the descriptor denies it.
    7, TUSB_DESC_CS_ENDPOINT, AS_EP_GENERAL, 0x01, 0x00, U16_TO_U8S_LE(0x0000),
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "UAC1 config descriptor length mismatch — check AUDIO_FUNC_DESC_LEN");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---------------------------------------------------------------------------
// String descriptors
// ---------------------------------------------------------------------------
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   // 0: English (0x0409)
    "Cobwebb",                    // 1: Manufacturer
    "1U Visualizer",              // 2: Product
    "WS349-0001",                 // 3: Serial
    "1U Visualizer Audio",        // 4: Audio control interface
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
