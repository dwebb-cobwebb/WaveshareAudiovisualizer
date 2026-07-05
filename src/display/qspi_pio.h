#ifndef _QSPI_PIO_H_
#define _QSPI_PIO_H_

#include "qspi.pio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

// Pin map for Waveshare RP2350-Touch-LCD-3.49
#define PIN_CS      25
#define PIN_SCLK    20
#define PIN_DIO0    21
#define PIN_DIO1    22
#define PIN_DIO2    23
#define PIN_DIO3    24
#define PIN_PWR_EN  37
#define PIN_RST     34

typedef struct pio_qspi {
    PIO     pio;
    uint8_t sm;
    uint8_t sm_4wire;
    uint8_t sm_1wire;
    uint8_t pin_cs;
    uint8_t pin_sclk;
    uint8_t pin_dio0;
    uint8_t pin_dio1;
    uint8_t pin_dio2;
    uint8_t pin_dio3;
    uint8_t pin_pwr_en;
    uint8_t pin_rst;
} pio_qspi_t;

extern pio_qspi_t qspi;

void QSPI_GPIO_Init(pio_qspi_t qspi);
void QSPI_Select(pio_qspi_t qspi);
void QSPI_Deselect(pio_qspi_t qspi);
void QSPI_PIO_Init(pio_qspi_t qspi);
void QSPI_1Wrie_Mode(pio_qspi_t *qspi);
void QSPI_4Wrie_Mode(pio_qspi_t *qspi);
void QSPI_DATA_Write(pio_qspi_t qspi, uint32_t val);
void QSPI_REGISTER_Write(pio_qspi_t qspi, uint32_t addr);
void QSPI_Pixel_Write(pio_qspi_t qspi, uint32_t addr);

#endif // _QSPI_PIO_H_
