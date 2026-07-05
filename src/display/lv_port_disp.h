#ifndef AV_LV_PORT_DISP_H
#define AV_LV_PORT_DISP_H

// Initializes the AXS15231B panel, LVGL draw buffers, display driver, and a
// 1 ms repeating timer that drives lv_tick_inc(). Call once on core0 after
// lv_init().
void lv_port_disp_init(void);

#endif // AV_LV_PORT_DISP_H
