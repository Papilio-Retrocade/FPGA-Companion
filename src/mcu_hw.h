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
void mcu_hw_spi_deinit(void);
#endif

// HW SPI interface
void mcu_hw_spi_begin(void);
unsigned char mcu_hw_spi_tx_u08(unsigned char b);
void mcu_hw_spi_end(void);

#ifdef ESP_PLATFORM
// UART1 — TangCore / BL616 protocol (GPIO43=TX→E14, GPIO44=RX←C9, 2 Mbps)
void mcu_hw_uart_init(void);
void mcu_hw_uart_tx_byte(uint8_t b);
void mcu_hw_uart_tx_buf(const uint8_t *buf, size_t len);
void mcu_hw_uart_tx_flush(void);   /* block until TX ring buffer is empty */
int  mcu_hw_uart_rx_available(void);
uint8_t mcu_hw_uart_rx_byte(void);
#endif

#endif // MCU_HW_H
