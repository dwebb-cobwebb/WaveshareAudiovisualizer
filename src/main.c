#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "lvgl.h"
#include "tusb.h"

#include "app.h"
#include "config.h"
#include "dsp/ringbuffer.h"
#include "dsp/analyzer.h"
#include "dsp/vis_state.h"
#include "usb/usb_audio.h"
#include "display/lv_port_disp.h"
#include "display/lv_port_indev.h"
#include "display/axs15231b.h"
#include "ui/ui_common.h"

// Cross-core audio ring (producer = USB poll timer on core0, consumer =
// analyzer on core1).
AudioRing g_audio_ring;

int main(void) {
    stdio_init_all();
    sleep_ms(200);
    printf("\n[1U Visualizer] boot — RP2350-Touch-LCD-3.49\n");

    ring_init(&g_audio_ring);
    vis_state_init();   // init double-buffer before core1 can publish

    // Display init first — axs_init() has ~750 ms of hardware-reset sleeps.
    // Must complete before USB starts so the long delay doesn't stall tud_task().
    lv_init();
    lv_port_disp_init();
    lv_indev_t *indev = lv_port_indev_init();
    ui_init(indev);
    printf("[ui] LVGL ready, %dx%d landscape\n", AV_DISP_W, AV_DISP_H);

    // DSP/FFT runs on core1.
    multicore_launch_core1(analyzer_core1_main);
    printf("[dsp] analyzer core launched\n");

    // USB last: enumeration window opens here and the main loop follows.
    usb_audio_init();
    printf("[usb] UAC1 device initialized (\"1U Visualizer\")\n");
    ui_status("waiting for host...");

    absolute_time_t next_ui = get_absolute_time();
    uint32_t last_frame_log = 0;
    absolute_time_t next_log = make_timeout_time_ms(1000);

    // Audio RX is serviced by a 500us hardware timer inside usb_audio (never
    // starved by the display blit); this loop only pumps EP0 control, LVGL,
    // and the meters.
    for (;;) {
        usb_audio_task();          // TinyUSB control plane + E12 IRQ re-pend
        lv_timer_handler();        // LVGL rendering / input

        // Update meters ~60 Hz.
        if (absolute_time_diff_us(get_absolute_time(), next_ui) <= 0) {
            ui_update();
            next_ui = make_timeout_time_ms(16);
        }

        // Once-per-second heartbeat — to UART and the on-screen idle status
        // line (hidden while streaming).
        if (absolute_time_diff_us(get_absolute_time(), next_log) <= 0) {
            VisualizerState vs;
            vis_acquire(&vs);
            bool mounted   = tud_mounted();
            bool streaming = usb_audio_streaming();
            uint32_t fps   = vs.frame_id - last_frame_log;
            usb_audio_dbg_t ud;
            usb_audio_debug(&ud);
            printf("[hb] mounted=%d streaming=%d rxpkts=%lu rxb=%lu frames=%lu (+%lu/s)\n",
                   (int)mounted, (int)streaming,
                   (unsigned long)ud.rx_pkts, (unsigned long)ud.rx_bytes,
                   (unsigned long)vs.frame_id, (unsigned long)fps);

            char line[64];
            snprintf(line, sizeof(line), "mnt=%d strm=%d rx=%lu frm=%lu",
                     (int)mounted, (int)streaming,
                     (unsigned long)ud.rx_pkts, (unsigned long)vs.frame_id);
            ui_status(line);

            last_frame_log = vs.frame_id;
            next_log = make_timeout_time_ms(1000);
        }
    }
}
