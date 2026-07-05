#include "display/axs15231b.h"
#include "display/qspi_pio.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

// ---------------------------------------------------------------------------
// DMA channel (claimed once at init, reused for every blit)
// ---------------------------------------------------------------------------
static int      s_dma_ch = -1;
static dma_channel_config s_dma_cfg;

// ---------------------------------------------------------------------------
// Backlight (PWM, 0-100 duty in the Waveshare scheme; we accept 0-255)
// ---------------------------------------------------------------------------
static uint s_bl_slice;

static void backlight_init(void) {
    gpio_set_function(AV_PIN_LCD_BL, GPIO_FUNC_PWM);
    s_bl_slice = pwm_gpio_to_slice_num(AV_PIN_LCD_BL);
    pwm_set_wrap(s_bl_slice, 100);
    pwm_set_chan_level(s_bl_slice, PWM_CHAN_A, 0);
    pwm_set_clkdiv(s_bl_slice, 50);
    pwm_set_enabled(s_bl_slice, true);
}

void axs_backlight(uint8_t level) {
    // level 0-255 → PWM 0-100
    uint32_t duty = (uint32_t)level * 100 / 255;
    pwm_set_chan_level(s_bl_slice, PWM_CHAN_A, 100 - duty);
}

// ---------------------------------------------------------------------------
// Panel init register table (from Waveshare LVGL demo, LCD_3in49.c)
// ---------------------------------------------------------------------------
typedef struct {
    int            cmd;
    const uint8_t *data;
    size_t         data_bytes;
    unsigned int   delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_cmds[] = {
    {0xBB, (const uint8_t[]){0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5}, 8, 0},
    {0xA0, (const uint8_t[]){0x00,0x30,0x00,0x02,0x00,0x00,0x04,0x3F,0x20,0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00}, 17, 0},
    {0xA2, (const uint8_t[]){0x30,0x19,0x60,0x64,0x9B,0x22,0x38,0x80,0xAC,0x28,0x7F,0x7F,0x7F,0x20,0xF8,0x10,0x02,0xFF,0xFF,0xF0,0x90,0x01,0x32,0xA0,0x91,0xC0,0x20,0x7F,0xFF,0x00,0x54}, 31, 0},
    {0xD0, (const uint8_t[]){0x80,0xAC,0x21,0x24,0x08,0x09,0x10,0x01,0x80,0x12,0xC2,0x00,0x22,0x22,0xAA,0x03,0x10,0x12,0x40,0x14,0x1E,0x51,0x15,0x00,0x40,0x10,0x00,0x03,0x7D,0x12}, 30, 0},
    {0xA3, (const uint8_t[]){0xA0,0x06,0xAA,0x00,0x08,0x02,0x0A,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x55,0x55}, 22, 0},
    {0xC1, (const uint8_t[]){0x33,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,0x00,0x41,0x00,0x53,0xFF,0xFF,0xFF,0x4F,0x52,0x00,0x4F,0x52,0x00,0x45,0x3B,0x0B,0x02,0x0D,0x00,0xFF,0x40}, 30, 0},
    {0xC3, (const uint8_t[]){0x00,0x00,0x00,0x50,0x03,0x00,0x00,0x00,0x01,0x80,0x01}, 11, 0},
    {0xC4, (const uint8_t[]){0x00,0x24,0x33,0x80,0x11,0xEA,0x64,0x32,0xC8,0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,0x44,0x50}, 29, 0},
    {0xC5, (const uint8_t[]){0x18,0x00,0x00,0x03,0xFE,0x08,0x68,0x30,0x10,0x10,0x88,0xDE,0x0D,0x08,0x0F,0x0F,0x01,0x08,0x68,0x30,0x10,0x10,0x00}, 23, 0},
    {0xC6, (const uint8_t[]){0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,0x22,0x12,0x22,0x01,0x00,0x00,0x3F,0x6A,0x18,0xC8,0x22}, 20, 0},
    {0xC7, (const uint8_t[]){0x50,0x32,0x28,0x00,0xA2,0x80,0x8F,0x00,0x80,0xFF,0x07,0x11,0x9F,0x6F,0xFF,0x24,0x0C,0x0D,0x0E,0x0F}, 20, 0},
    {0xC9, (const uint8_t[]){0x33,0x44,0x44,0x01}, 4, 0},
    {0xCF, (const uint8_t[]){0x2C,0x1E,0x88,0x58,0x13,0x18,0x56,0x18,0x1E,0x68,0xF8,0x00,0x66,0x0D,0x22,0xC4,0x0C,0x77,0x22,0x44,0xAA,0x55,0x04,0x04,0x12,0xA0,0x08}, 28, 0},
    {0xD5, (const uint8_t[]){0x50,0x60,0x8A,0x00,0x35,0x04,0x60,0x10,0x03,0x03,0x03,0x00,0x04,0x02,0x13,0x46,0x03,0x03,0x03,0x03,0x86,0x00,0x00,0x00,0x80,0x52,0x7C,0x00,0x00,0x00}, 30, 0},
    {0xD6, (const uint8_t[]){0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x00,0x00,0x01,0x83,0x03,0x03,0x33,0x03,0x03,0x33,0x3F,0x03,0x03,0x03,0x20,0x20,0x00,0x24,0x51,0x23,0x01,0x00}, 30, 0},
    {0xD7, (const uint8_t[]){0x18,0x1A,0x1B,0x1F,0x0A,0x08,0x0E,0x0C,0x00,0x1F,0x1D,0x1F,0x50,0x60,0x04,0x00,0x1F,0x1F,0x1F}, 19, 0},
    {0xD8, (const uint8_t[]){0x18,0x1A,0x1B,0x1F,0x0B,0x09,0x0F,0x0D,0x01,0x1F,0x1D,0x1F}, 12, 0},
    {0xD9, (const uint8_t[]){0x0F,0x09,0x0B,0x1F,0x18,0x19,0x1F,0x01,0x1E,0x1D,0x1F}, 11, 0},
    {0xDD, (const uint8_t[]){0x0E,0x08,0x0A,0x1F,0x18,0x19,0x1F,0x00,0x1E,0x1A,0x1F}, 11, 0},
    {0xDF, (const uint8_t[]){0x44,0x73,0x4B,0x69,0x00,0x0A,0x02,0x90}, 8, 0},
    {0xE0, (const uint8_t[]){0x35,0x08,0x19,0x1C,0x0C,0x09,0x13,0x2A,0x54,0x21,0x0B,0x15,0x13,0x25,0x27,0x08,0x00}, 17, 0},
    {0xE1, (const uint8_t[]){0x3E,0x08,0x19,0x1C,0x0C,0x08,0x13,0x2A,0x54,0x21,0x0B,0x14,0x13,0x26,0x27,0x08,0x0F}, 17, 0},
    {0xE2, (const uint8_t[]){0x19,0x20,0x0A,0x11,0x09,0x06,0x11,0x25,0xD4,0x22,0x0B,0x13,0x12,0x2D,0x32,0x2F,0x03}, 17, 0},
    {0xE3, (const uint8_t[]){0x38,0x20,0x0A,0x11,0x09,0x06,0x11,0x25,0xC4,0x21,0x0A,0x12,0x11,0x2C,0x32,0x2F,0x27}, 17, 0},
    {0xE4, (const uint8_t[]){0x19,0x20,0x0D,0x14,0x0D,0x08,0x12,0x2A,0xD4,0x26,0x0E,0x15,0x13,0x34,0x39,0x2F,0x03}, 17, 0},
    {0xE5, (const uint8_t[]){0x38,0x20,0x0D,0x13,0x0D,0x07,0x12,0x29,0xC4,0x25,0x0D,0x15,0x12,0x33,0x39,0x2F,0x27}, 17, 0},
    {0xA4, (const uint8_t[]){0x85,0x85,0x95,0x82,0xAF,0xAA,0xAA,0x80,0x10,0x30,0x40,0x40,0x20,0xFF,0x60,0x30}, 16, 0},
    {0xA4, (const uint8_t[]){0x85,0x85,0x95,0x85}, 4, 0},
    {0xBB, (const uint8_t[]){0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 20},
};

// ---------------------------------------------------------------------------
// Send one command + data bytes via QSPI
// ---------------------------------------------------------------------------
static void send_cmd(int cmd, const uint8_t *data, size_t len) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, (uint32_t)cmd);
    for (size_t i = 0; i < len; i++) {
        QSPI_DATA_Write(qspi, data[i]);
    }
    QSPI_Deselect(qspi);
}

// ---------------------------------------------------------------------------
// axs_init
// ---------------------------------------------------------------------------
void axs_init(void) {
    // QSPI GPIO + PIO
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);

    // Hardware reset
    gpio_put(PIN_RST, 1); sleep_ms(200);
    gpio_put(PIN_RST, 0); sleep_ms(200);
    gpio_put(PIN_RST, 1); sleep_ms(200);

    // Panel register init sequence
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const lcd_init_cmd_t *c = &s_init_cmds[i];
        if (c->data_bytes > 0) {
            send_cmd(c->cmd, c->data, c->data_bytes);
        } else {
            // Command with no data (Sleep Out, Display On)
            QSPI_Select(qspi);
            QSPI_REGISTER_Write(qspi, (uint32_t)c->cmd);
            QSPI_Deselect(qspi);
        }
        if (c->delay_ms > 0) sleep_ms(c->delay_ms);
    }

    // DMA — claim once, configure for 8-bit PIO TX
    s_dma_ch  = dma_claim_unused_channel(true);
    s_dma_cfg = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&s_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_dma_cfg, true);
    channel_config_set_write_increment(&s_dma_cfg, false);

    // No MADCTL (0x36) — the Waveshare reference never sets it. The panel runs in
    // its default portrait orientation: 172 columns (x) wide, 640 rows (y) tall.
    // CASET addresses columns 0..171, RASET addresses rows 0..639. "Landscape" is
    // achieved by software-transposing content into this portrait GRAM, not by MADCTL.

    // Backlight
    backlight_init();
    axs_backlight(AV_BACKLIGHT_LEVEL);
}

// ---------------------------------------------------------------------------
// axs_orientation_test — definitive GRAM->physical mapping diagnostic.
//
// Paints the panel DIRECTLY in portrait coordinates (no MADCTL, no LVGL):
//   rows   0..159 = RED, 160..319 = GREEN, 320..479 = BLUE, 480..639 = YELLOW
//   columns 0..5  = WHITE stripe (marks the column-0 edge)
// Streams row-major (640 rows x 172 cols) so we observe exactly which physical
// edge is row 0 vs row 639 and which edge is column 0.
// ---------------------------------------------------------------------------
void axs_orientation_test(void) {
    // RGB565, big-endian byte order as the panel expects (hi byte first).
    const uint16_t RED    = 0xF800;
    const uint16_t GREEN  = 0x07E0;
    const uint16_t BLUE   = 0x001F;
    const uint16_t YELLOW = 0xFFE0;
    const uint16_t WHITE  = 0xFFFF;

    axs_set_window(0, 0, AV_DISP_NATIVE_W - 1, AV_DISP_NATIVE_H - 1);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2C);
    for (uint32_t row = 0; row < AV_DISP_NATIVE_H; row++) {
        uint16_t band = (row < 160) ? RED
                      : (row < 320) ? GREEN
                      : (row < 480) ? BLUE
                                    : YELLOW;
        for (uint32_t col = 0; col < AV_DISP_NATIVE_W; col++) {
            uint16_t c = (col < 6) ? WHITE : band;
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(c >> 8)   << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)(c & 0xFF) << 24);
        }
    }
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) { tight_loop_contents(); }
    QSPI_Deselect(qspi);
}

// ---------------------------------------------------------------------------
// axs_set_window  — portrait coordinates (0..171 x, 0..639 y)
// Endpoints are inclusive (LVGL area->x2/y2 convention).
// The 0x02 RAMWR "arm" step at the end is required: the panel's write cursor
// only updates to (x0, y0) when 0x2C is sent via the single-wire 0x02
// instruction. The subsequent 0x32 quad-write in axs_blit_* then streams
// pixel data from that armed position. Without this step every 0x32 write
// defaults to row 0 regardless of the RASET value.
// ---------------------------------------------------------------------------
void axs_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // CASET (column = x)
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2A);
    QSPI_DATA_Write(qspi, x0 >> 8);
    QSPI_DATA_Write(qspi, x0 & 0xFF);
    QSPI_DATA_Write(qspi, x1 >> 8);
    QSPI_DATA_Write(qspi, x1 & 0xFF);
    QSPI_Deselect(qspi);

    // RASET (row = y)
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2B);
    QSPI_DATA_Write(qspi, y0 >> 8);
    QSPI_DATA_Write(qspi, y0 & 0xFF);
    QSPI_DATA_Write(qspi, y1 >> 8);
    QSPI_DATA_Write(qspi, y1 & 0xFF);
    QSPI_Deselect(qspi);

    // Arm the write cursor to (x0, y0) via the single-wire 0x02 instruction.
    // Mirrors LCD_3IN49_SetWindows in the Waveshare demo.
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x2C);
    QSPI_Deselect(qspi);
}

// ---------------------------------------------------------------------------
// Raw pixel streaming — full-panel RAMWR session for renderers that generate
// pixels on the fly (see mode_tunnel.c). Between begin() and end() the caller
// pushes 2 bytes per pixel with axs_stream_px(), in portrait scan order.
// ---------------------------------------------------------------------------
void axs_stream_begin(void) {
    axs_set_window(0, 0, AV_DISP_NATIVE_W - 1, AV_DISP_NATIVE_H - 1);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2C);
}

void axs_stream_end(void) {
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) { tight_loop_contents(); }
    QSPI_Deselect(qspi);
}

// ---------------------------------------------------------------------------
// Pixel blit — RAMWR + DMA
// ---------------------------------------------------------------------------
void axs_blit_async(const uint16_t *px, uint32_t count) {
    // Switch dreq to PIO TX for this SM
    channel_config_set_dreq(&s_dma_cfg, pio_get_dreq(qspi.pio, qspi.sm, true));

    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2C);  // RAMWR in 4-wire pixel mode

    dma_channel_configure(s_dma_ch, &s_dma_cfg,
                          &qspi.pio->txf[qspi.sm],   // write: PIO TX FIFO
                          px,                          // read: pixel buffer
                          count * 2,                  // byte count (RGB565 = 2 B/px)
                          true);                       // start immediately
}

bool axs_blit_done(void) {
    if (dma_channel_is_busy(s_dma_ch)) return false;
    // DMA counter hit zero but PIO TX FIFO may still hold up to 8 unshifted bytes.
    // Wait for it to drain before deselecting CS or the tail bytes are lost.
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) { tight_loop_contents(); }
    QSPI_Deselect(qspi);
    return true;
}

void axs_blit(const uint16_t *px, uint32_t count) {
    axs_blit_async(px, count);
    while (!axs_blit_done()) { tight_loop_contents(); }
}

// Blocking PIO write — no DMA, byte-by-byte via pio_sm_put_blocking.
void axs_blit_pio(const uint16_t *px, uint32_t count) {
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2C);
    const uint8_t *b = (const uint8_t *)px;
    for (uint32_t i = 0; i < count * 2; i++) {
        pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)b[i] << 24);
    }
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) { tight_loop_contents(); }
    QSPI_Deselect(qspi);
}

// Transpose-blit: 640x172 landscape logical frame -> 172x640 portrait panel.
//
// The panel is portrait (no MADCTL): GRAM is 172 columns (c) x 640 rows (r),
// streamed row-major from row 0. The long axis is the row axis. The base mapping
// is physical(row=r, col=c) <- logical(lx=r, ly=c); AV_LANDSCAPE_FLIP_X / _Y
// independently reverse each logical axis to land on the correct rotation for
// this panel's scan handedness (the row/col scan directions are not a pure
// rotation, so both flips are tuned against hardware).
//
// Bytes are sent in LVGL's in-memory order (matching the working portrait path
// and the Waveshare reference, which DMAs lv_color_t bytes directly).
void axs_blit_pio_landscape(const uint16_t *px) {
    axs_set_window(0, 0, AV_DISP_NATIVE_W - 1, AV_DISP_NATIVE_H - 1);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2C);
    const uint8_t *base = (const uint8_t *)px;
    for (uint32_t r = 0; r < AV_DISP_NATIVE_H; r++) {
        for (uint32_t c = 0; c < AV_DISP_NATIVE_W; c++) {
#if AV_LANDSCAPE_FLIP_X
            uint32_t lx = (AV_DISP_W - 1) - r;
#else
            uint32_t lx = r;
#endif
#if AV_LANDSCAPE_FLIP_Y
            uint32_t ly = (AV_DISP_H - 1) - c;
#else
            uint32_t ly = c;
#endif
            const uint8_t *p = base + 2u * (ly * AV_DISP_W + lx);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)p[0] << 24);
            pio_sm_put_blocking(qspi.pio, qspi.sm, (uint32_t)p[1] << 24);
        }
    }
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) { tight_loop_contents(); }
    QSPI_Deselect(qspi);
}
