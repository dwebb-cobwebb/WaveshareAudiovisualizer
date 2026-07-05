#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Board / MCU
// ---------------------------------------------------------------------------
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU            OPT_MCU_RP2040   // RP2350 uses the rp2040 USB DCD in TinyUSB
#endif

#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))
#endif

// ---------------------------------------------------------------------------
// Device stack config
//
// The audio function is implemented as a hand-rolled UAC 1.0 class driver
// (see usb_audio.c, registered via usbd_app_driver_get_cb). TinyUSB's built-in
// audio class only supports UAC 2.0 (audio_device.c verifies bInterfaceProtocol
// == AUDIO_INT_PROTOCOL_CODE_V2), and Windows' usbaudio2.sys demands
// GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS for full-speed devices — which this host
// answers NOT_SUPPORTED, killing the stream. UAC1 (usbaudio.sys) never does
// that, so we drop the built-in audio class entirely.
// ---------------------------------------------------------------------------
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_AUDIO           0
// CDC-ACM serial: host pushes wall-clock time (scripts/set_clock.ps1); also a
// debug channel. Handled by TinyUSB's built-in class driver alongside the
// custom UAC1 driver.
#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  64
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
