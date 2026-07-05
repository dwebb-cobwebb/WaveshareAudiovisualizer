#include "display/qspi_pio.h"
#include "pico/stdlib.h"

pio_qspi_t qspi = {
    .pio       = pio0,
    .sm        = 0,
    .sm_4wire  = 0,
    .sm_1wire  = 1,
    .pin_cs    = PIN_CS,
    .pin_sclk  = PIN_SCLK,
    .pin_dio0  = PIN_DIO0,
    .pin_dio1  = PIN_DIO1,
    .pin_dio2  = PIN_DIO2,
    .pin_dio3  = PIN_DIO3,
    .pin_pwr_en = PIN_PWR_EN,
    .pin_rst   = PIN_RST,
};

void QSPI_GPIO_Init(pio_qspi_t qspi) {
    gpio_init(qspi.pin_cs);
    gpio_pull_down(qspi.pin_cs);
    gpio_set_dir(qspi.pin_cs, GPIO_OUT);
    gpio_put(qspi.pin_cs, 1);

    gpio_init(qspi.pin_pwr_en);
    gpio_set_dir(qspi.pin_pwr_en, GPIO_OUT);
    gpio_put(qspi.pin_pwr_en, 1);

    gpio_init(qspi.pin_rst);
    gpio_set_dir(qspi.pin_rst, GPIO_OUT);
}

void QSPI_Select(pio_qspi_t qspi) {
    gpio_put(qspi.pin_cs, 0);
}

void QSPI_Deselect(pio_qspi_t qspi) {
    gpio_put(qspi.pin_cs, 1);
}

void QSPI_PIO_Init(pio_qspi_t qspi) {
    uint offset = pio_add_program(qspi.pio, &qspi_4wire_data_program);
    qspi_4wire_data_program_init(qspi.pio, qspi.sm_4wire, offset, PIN_SCLK, PIN_DIO0, 4);

    pio_sm_set_enabled(qspi.pio, qspi.sm_4wire, false);
    pio_sm_set_enabled(qspi.pio, qspi.sm_1wire, false);
}

void QSPI_4Wrie_Mode(pio_qspi_t *qspi) {
    pio_sm_set_enabled(qspi->pio, qspi->sm_4wire, true);
    pio_sm_set_enabled(qspi->pio, qspi->sm_1wire, false);
    qspi->sm = qspi->sm_4wire;
}

void QSPI_1Wrie_Mode(pio_qspi_t *qspi) {
    pio_sm_set_enabled(qspi->pio, qspi->sm_4wire, false);
    pio_sm_set_enabled(qspi->pio, qspi->sm_1wire, true);
    qspi->sm = qspi->sm_1wire;
}

static void QSPI_PIO_Write(pio_qspi_t qspi, uint32_t val) {
    pio_sm_put_blocking(qspi.pio, qspi.sm, val << 24);
}

void QSPI_DATA_Write(pio_qspi_t qspi, uint32_t val) {
    uint8_t cmd_buf[4];
    for (int i = 0; i < 4; i++) {
        uint8_t bit1 = (val & (1 << (2 * i))) ? 1 : 0;
        uint8_t bit2 = (val & (1 << (2 * i + 1))) ? 1 : 0;
        cmd_buf[3 - i] = bit1 | (bit2 << 4);
    }
    for (int i = 0; i < 4; i++) {
        QSPI_PIO_Write(qspi, cmd_buf[i]);
    }
}

static void QSPI_CMD_Write(pio_qspi_t qspi, uint32_t val) {
    uint8_t cmd_buf[4];
    for (int i = 0; i < 4; i++) {
        uint8_t bit1 = (val & (1 << (2 * i))) ? 1 : 0;
        uint8_t bit2 = (val & (1 << (2 * i + 1))) ? 1 : 0;
        cmd_buf[3 - i] = bit1 | (bit2 << 4);
    }
    for (int i = 0; i < 4; i++) {
        QSPI_PIO_Write(qspi, cmd_buf[i]);
    }
}

void QSPI_REGISTER_Write(pio_qspi_t qspi, uint32_t addr) {
    QSPI_CMD_Write(qspi, 0x02);
    QSPI_DATA_Write(qspi, 0x00);
    QSPI_DATA_Write(qspi, addr);
    QSPI_DATA_Write(qspi, 0x00);
}

void QSPI_Pixel_Write(pio_qspi_t qspi, uint32_t addr) {
    QSPI_CMD_Write(qspi, 0x32);
    QSPI_DATA_Write(qspi, 0x00);
    QSPI_DATA_Write(qspi, addr);
    QSPI_DATA_Write(qspi, 0x00);
}
