#include "display/touch_axs.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

// Touch controller uses I2C0 (GPIO 32 SDA, GPIO 33 SCL).
#define TP_I2C  i2c0

// The controller's report is only refreshed around INT events (active-low
// pulse per report). Free-polling re-reads the STALE last report — after the
// first touch the chip keeps answering "finger down", LVGL believes the
// screen is held pressed forever, and no further tap ever registers. The
// Waveshare reference reads only on the INT edge; we mirror that, but defer
// the slow I2C transaction to thread context (LVGL's read cb) so it can never
// delay the audio poll timer IRQ.
static volatile bool s_tp_event = false;

static void tp_int_cb(uint gpio, uint32_t events) {
    (void)events;
    if (gpio == AV_PIN_TP_INT) s_tp_event = true;
}

void touch_init(void) {
    i2c_init(TP_I2C, 400 * 1000);
    gpio_set_function(AV_PIN_TP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(AV_PIN_TP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(AV_PIN_TP_SDA);
    gpio_pull_up(AV_PIN_TP_SCL);

    // INT pin: falling-edge interrupt marks "new report available".
    gpio_init(AV_PIN_TP_INT);
    gpio_set_dir(AV_PIN_TP_INT, GPIO_IN);
    gpio_pull_up(AV_PIN_TP_INT);
    gpio_set_irq_enabled_with_callback(AV_PIN_TP_INT, GPIO_IRQ_EDGE_FALL, true, tp_int_cb);
}

// One physical press must read as ONE continuous LVGL press. INT pulses
// arrive per-report (slower than LVGL's poll), so between pulses we keep
// reporting PRESSED with the cached position for a short hold window;
// otherwise a single swipe fragments into several micro-presses and fires
// multiple gestures.
#define TP_HOLD_MS 80
static uint16_t s_last_x, s_last_y;
static uint32_t s_last_evt_ms;
static bool     s_held;

bool touch_read(uint16_t *x, uint16_t *y) {
    if (!s_tp_event) {
        // No new report: sustain the press briefly, then release.
        if (s_held && (to_ms_since_boot(get_absolute_time()) - s_last_evt_ms) < TP_HOLD_MS) {
            *x = s_last_x; *y = s_last_y;
            return true;
        }
        s_held = false;
        return false;
    }
    s_tp_event = false;

    // Write 11-byte command, read 32-byte report (from Waveshare Touch.c)
    static const uint8_t cmd[11] = {
        0xB5, 0xAB, 0xA5, 0x5A,
        0x00, 0x00, 0x00, 0x0E,
        0x00, 0x00, 0x00
    };
    uint8_t buf[32] = {0};

    if (i2c_write_blocking(TP_I2C, AV_TP_I2C_ADDR, cmd, sizeof(cmd), true)  < 0) return false;
    if (i2c_read_blocking (TP_I2C, AV_TP_I2C_ADDR, buf, sizeof(buf), false) < 0) return false;

    uint8_t fingers = buf[1];
    if (fingers == 0) { s_held = false; return false; }

    // Raw coords: long axis (0..640), short axis (0..172)
    uint16_t pt_long  = ((buf[2] & 0x0F) << 8) | buf[3];
    uint16_t pt_short = ((buf[4] & 0x0F) << 8) | buf[5];

    // Return landscape (640×172) coordinates to match LVGL's landscape canvas.
    // Panel is portrait (172×640) driven with 90° CW pixel rotation, so:
    //   landscape_x = 639 - pt_long   (long axis inverted → landscape X)
    //   landscape_y = pt_short         (short axis → landscape Y)
    // If touch appears mirrored: try landscape_x = pt_long, or landscape_y = 171-pt_short.
    *x = (pt_long  < AV_DISP_W) ? (AV_DISP_W - 1 - pt_long)  : 0;
    *y = (pt_short < AV_DISP_H) ? pt_short                    : (AV_DISP_H - 1);
    s_last_x = *x; s_last_y = *y;
    s_last_evt_ms = to_ms_since_boot(get_absolute_time());
    s_held = true;
    return true;
}
