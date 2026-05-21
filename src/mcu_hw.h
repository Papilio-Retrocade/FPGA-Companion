#ifndef MCU_HW_H
#define MCU_HW_H

#include <stdio.h>
#include <inttypes.h>

#define LOGO "\033[1;33m"\
  "  __  __ _ ___ _____             _  _               \r\n"\
  " |  \\/  (_) __|_   _|__ _ _ _  _| \\| |__ _ _ _  ___ \r\n"\
  " | |\\/| | \\__ \\ | |/ -_) '_| || | .` / _` | ' \\/ _ \\\r\n"\
  " |_|  |_|_|___/ |_|\\___|_|  \\_, |_|\\_\\__,_|_||_\\___/\r\n"\
  "                            |__/                    \r\n\033[0m"
  
void mcu_hw_init(void);
void mcu_hw_main_loop(void);

void mcu_hw_irq_ack(void);
void mcu_hw_reset(void);
#ifdef ESP_PLATFORM
#include "esp_err.h"
void mcu_hw_fpga_reset(void);
/* Brief RECONFIG_N pulse for JTAG recovery (~5 ms low, no post-delay).
 * Caller can race a JTAG CONFIG_ENABLE instruction in before the FPGA's
 * configuration FSM finishes auto-loading a (possibly corrupt) bitstream
 * from SPI flash. */
void mcu_hw_fpga_reset_brief(void);
void mcu_hw_erase_flash_region(uint32_t addr, uint32_t size);
void mcu_hw_write_flash(uint32_t addr, uint8_t *data, uint32_t size);
void mcu_hw_read_flash(uint32_t addr, uint8_t *data, uint32_t size);
void mcu_hw_spi_flash_begin(void);
void mcu_hw_spi_flash_end(void);
esp_err_t mcu_hw_reinit_flash(void);
#endif

// HW SPI interface
void mcu_hw_spi_begin(void);
unsigned char mcu_hw_spi_tx_u08(unsigned char b);
void mcu_hw_spi_end(void);

#endif // MCU_HW_H
