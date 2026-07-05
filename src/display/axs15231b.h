#ifndef AV_AXS15231B_H
#define AV_AXS15231B_H

#include <stdint.h>
#include <stdbool.h>

// AXS15231B QSPI LCD driver.
//
// IMPORTANT: the QSPI pin map, panel init register sequence, and color-window
// command bytes are board-specific. Port them from the Waveshare
// RP2350-Touch-LCD-3.49 demo (its axs15231b driver source) into this file.
// Pins are declared as placeholders in src/config.h (AV_PIN_LCD_*).

void axs_init(void);

// Diagnostic: paint portrait GRAM with row-quarter colour bands (RED/GREEN/BLUE/
// YELLOW for rows 0..639) plus a WHITE stripe on columns 0..5, to map GRAM
// coordinates to physical screen position. No MADCTL, no LVGL.
void axs_orientation_test(void);

// Set the active window (in landscape coordinates) for the next pixel write.
void axs_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Push a block of RGB565 pixels for the current window over QSPI.
// `px` length must equal (x1-x0+1)*(y1-y0+1). Blocks until the DMA completes
// (or call axs_flush_async + axs_flush_done for overlap).
void axs_blit(const uint16_t *px, uint32_t count);

// Same as axs_blit but uses blocking PIO writes — no DMA.
void axs_blit_pio(const uint16_t *px, uint32_t count);

// Full-frame blit for MADCTL MV=1 landscape mode.
// px must be a 640×172 landscape row-major buffer (LVGL layout).
// Internally transposes to portrait write order and sets the portrait window.
void axs_blit_pio_landscape(const uint16_t *px);

// Async variant: kicks a DMA transfer and returns immediately.
void axs_blit_async(const uint16_t *px, uint32_t count);

// Raw full-panel pixel-stream session (portrait scan order, 2 B/px pushed by
// the caller into the QSPI PIO FIFO). For on-the-fly renderers.
void axs_stream_begin(void);
void axs_stream_end(void);
bool axs_blit_done(void);

// Backlight 0..255 (PWM).
void axs_backlight(uint8_t level);

#endif // AV_AXS15231B_H
