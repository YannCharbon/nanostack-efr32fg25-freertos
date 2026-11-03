/*
 * Board-level power settings for low-power measurements.
 *
 * board_flash_shutdown() (FW_BOARD_FLASH_DPD): the BRD4270B/BRD4271A radio
 * boards carry an MX25R8035F SPI flash on EUSART1's pins (PC0 MOSI, PC1 MISO,
 * PC2 SCLK, PC3 CS; the SDK's sl_mx25_flash_shutdown_eusart_config.h for
 * these boards). The firmware does not use it, and in standby it draws
 * several µA from the same supply as the SoC, which the AEM measures. This
 * sends it the Deep Power-Down command (0xB9), bit-banged on the GPIOs, as
 * the SDK component mx25_flash_shutdown does with the EUSART: release from
 * deep power-down (CS pulse), then 0xB9, then the pins are disabled with CS
 * pulled high.
 *
 * board_test_em4() (FW_TEST_EM4_AFTER_S, test only): waits, then puts the
 * chip in EM4 for good, to measure the board's residual current with the AEM.
 * Only a power cycle or a pin reset brings it back.
 *
 * board_test_em2_idle() (FW_TEST_EM2_IDLE_S, test only): waits, then stops
 * here for good with nothing running (no radio, no stack), so the power
 * manager keeps the chip in EM2: the bare EM2 floor of the kit. With
 * FW_TEST_EM2_VCOM_OFF, the VCOM pins (PA8 TX, PA9 RX) and the board's VCOM
 * enable (PA11) are switched off first, to see what the UART towards the
 * board controller draws.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "em_device.h"
#include "em_gpio.h"
#include "em_emu.h"
#include "sl_clock_manager.h"
#include "FreeRTOS.h"
#include "task.h"

#include "ns_port.h"

#define FL_PORT gpioPortC
#define FL_MOSI 0
#define FL_MISO 1
#define FL_SCLK 2
#define FL_CS   3

static void spin_us(uint32_t us)
{
    /* Generous: at most one cycle per iteration at SystemCoreClock */
    uint32_t n = (SystemCoreClockGet() / 1000000u) * us;
    for (volatile uint32_t i = 0; i < n; i++) {
    }
}

void board_flash_shutdown(void)
{
    sl_clock_manager_enable_bus_clock(SL_BUS_CLOCK_GPIO);
    GPIO_PinModeSet(FL_PORT, FL_MOSI, gpioModePushPull, 1);
    GPIO_PinModeSet(FL_PORT, FL_MISO, gpioModeInput, 0);
    GPIO_PinModeSet(FL_PORT, FL_SCLK, gpioModePushPull, 0);
    GPIO_PinModeSet(FL_PORT, FL_CS, gpioModePushPull, 1);
    vTaskDelay(pdMS_TO_TICKS(2));          /* tVSL 800 µs */

    GPIO_PinOutClear(FL_PORT, FL_CS);      /* release from deep power-down */
    spin_us(20);                           /* tCRDP */
    GPIO_PinOutSet(FL_PORT, FL_CS);
    spin_us(50);                           /* tRDP 35 µs */

    GPIO_PinOutClear(FL_PORT, FL_CS);
    spin_us(1);
    for (int b = 7; b >= 0; b--) {         /* SPI mode 0, MSB first: 0xB9 */
        if ((0xB9 >> b) & 1) {
            GPIO_PinOutSet(FL_PORT, FL_MOSI);
        } else {
            GPIO_PinOutClear(FL_PORT, FL_MOSI);
        }
        spin_us(1);
        GPIO_PinOutSet(FL_PORT, FL_SCLK);
        spin_us(1);
        GPIO_PinOutClear(FL_PORT, FL_SCLK);
    }
    spin_us(1);
    GPIO_PinOutSet(FL_PORT, FL_CS);        /* the command executes on CS high */
    spin_us(50);                           /* tDP 10 µs */

    GPIO_PinModeSet(FL_PORT, FL_MOSI, gpioModeDisabled, 0);
    GPIO_PinModeSet(FL_PORT, FL_MISO, gpioModeDisabled, 0);
    GPIO_PinModeSet(FL_PORT, FL_SCLK, gpioModeDisabled, 1);
    GPIO_PinModeSet(FL_PORT, FL_CS, gpioModeDisabled, 1);   /* disabled, pull-up */
}

void board_test_em2_idle(uint32_t after_s, int vcom_off)
{
    vTaskDelay(pdMS_TO_TICKS(after_s * 1000u));
    if (vcom_off) {
        GPIO_PinModeSet(gpioPortA, 8, gpioModeDisabled, 0);
        GPIO_PinModeSet(gpioPortA, 9, gpioModeDisabled, 0);
        GPIO_PinModeSet(gpioPortA, 11, gpioModePushPull, 0);
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void board_test_em4(uint32_t after_s)
{
    vTaskDelay(pdMS_TO_TICKS(after_s * 1000u));
    taskDISABLE_INTERRUPTS();
    EMU_EnterEM4();
    for (;;) {
    }
}
