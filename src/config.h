#ifndef AV_CONFIG_H
#define AV_CONFIG_H

// ---------------------------------------------------------------------------
// Audio / DSP
// ---------------------------------------------------------------------------
#define AV_SAMPLE_RATE_HZ   48000   // matches RME; UAC2 negotiates this
#define AV_CHANNELS         2       // stereo
#define AV_FFT_SIZE         1024    // power of two; ~21.3 ms window, ~46.9 Hz bins
#define AV_FFT_BINS         (AV_FFT_SIZE / 2)
#define AV_NUM_BANDS        31      // log-spaced display bands, 20 Hz .. 20 kHz

// Ring buffer holds interleaved stereo frames feeding the analyzer.
#define AV_RING_FRAMES      4096    // frames (L+R pairs); power of two

#define AV_BAND_FREQ_LOW    20.0f
#define AV_BAND_FREQ_HIGH   20000.0f

// Smoothing / ballistics (per analysis frame, ~ AV_SAMPLE_RATE/AV_FFT_SIZE Hz)
#define AV_BAND_ATTACK      0.60f   // 0..1, higher = snappier rise
#define AV_BAND_DECAY       0.12f   // 0..1, higher = faster fall
#define AV_PEAK_HOLD_MS     2000    // clip/peak hold time at 0 dBFS

// ---------------------------------------------------------------------------
// Display (AXS15231B) — native panel is 172(W) x 640(H) portrait.
// We render in landscape: 640(W) x 172(H).
// ---------------------------------------------------------------------------
#define AV_DISP_W           640
#define AV_DISP_H           172
#define AV_DISP_NATIVE_W    172
#define AV_DISP_NATIVE_H    640

// Landscape orientation. The panel is physically portrait (172x640) with GRAM
// row 0 at one long edge. The flush transposes the 640x172 logical frame onto
// it. The two axes are flipped independently so the result can be brought to
// the correct rotation regardless of the panel's row/column scan handedness:
//   FLIP_X — flip the long (horizontal / row) axis
//   FLIP_Y — flip the short (vertical / column) axis
#define AV_LANDSCAPE_FLIP_X 0
#define AV_LANDSCAPE_FLIP_Y 1

// Layout: spectrum on the left, stereo meters reserved on the right.
#define AV_METER_AREA_W     120
#define AV_SPECTRUM_W       (AV_DISP_W - AV_METER_AREA_W)

// Backlight brightness when awake (0..255 into axs_backlight).
#define AV_BACKLIGHT_LEVEL      200
// Display auto-sleep after this long without an active audio stream.
// Swipe down sleeps immediately; touch or a new stream wakes.
#define AV_DISPLAY_TIMEOUT_MS   (5u * 60u * 1000u)

// ---------------------------------------------------------------------------
// Pin map — Waveshare RP2350-Touch-LCD-3.49 (from Waveshare LVGL demo)
// ---------------------------------------------------------------------------
// QSPI display (AXS15231B) via PIO0
#define AV_PIN_LCD_CS       25
#define AV_PIN_LCD_SCK      20
#define AV_PIN_LCD_D0       21
#define AV_PIN_LCD_D1       22
#define AV_PIN_LCD_D2       23
#define AV_PIN_LCD_D3       24
#define AV_PIN_LCD_RST      34
#define AV_PIN_LCD_TE       (-1)   // not wired
#define AV_PIN_LCD_BL       36     // backlight (PWM)
#define AV_PIN_LCD_PWR_EN   37     // display power enable (active high)

// Capacitive touch (CST816 compatible) — I2C0
#define AV_PIN_TP_SDA       32
#define AV_PIN_TP_SCL       33
#define AV_PIN_TP_INT       11
#define AV_PIN_TP_RST       (-1)   // not exposed
#define AV_TP_I2C_ADDR      0x3B

#endif // AV_CONFIG_H
