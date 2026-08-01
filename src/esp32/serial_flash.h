/*
 * serial_flash.h - FPGA bitstream flashing over USB serial (OTA-over-IP fallback)
 *
 * Lets a host tool (the hosted web flasher, or a plain pyserial script) write
 * an FPGA bitstream to the board over the same USB serial console used for
 * Step 1 (ESP32 flash) and WiFi provisioning, without needing a known IP.
 *
 * Protocol (sent over the console UART/USB, same channel as wifi_provision.h):
 *   Host -> Device:  FPGA_FLASH_BEGIN <target> <size>\n
 *                      target: "flash" (SPI, persistent) or "sram" (JTAG, volatile)
 *                      size:   decimal byte count of the raw bitstream
 *   Device -> Host:  READY\r\n                 (or  ERROR <reason>\r\n)
 *   Host -> Device:  <raw binary bytes, exactly <size> of them, no framing>
 *   Device -> Host:  PROGRESS <bytes_written>\r\n   (periodically, every ~64 KB)
 *   Device -> Host:  FPGA_FLASH_OK\r\n         (or  FPGA_FLASH_ERROR <reason>\r\n)
 *
 * Only one command is handled per call, from the single stdin-reading task in
 * wifi_provision.c - this module never reads stdin outside of a command it is
 * actively handling, so there's no risk of two tasks racing for the console.
 */

#ifndef SERIAL_FLASH_H
#define SERIAL_FLASH_H

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_OTA_ENABLE)

#include <stdbool.h>

/**
 * Inspect a line already read from stdin by the console dispatcher. If it is
 * a "FPGA_FLASH_BEGIN ..." command, handles the entire exchange (reads the
 * raw bitstream bytes itself, writes it to flash or FPGA SRAM via JTAG, and
 * replies with the protocol above) before returning.
 *
 * @param line  Newline-stripped line just read from stdin.
 * @return true if `line` was a serial_flash command (handled, whether it
 *         succeeded or failed) - caller should not process it further.
 *         false if `line` was not recognised - caller should keep dispatching.
 */
bool serial_flash_try_handle_command(const char *line);

#else

static inline bool serial_flash_try_handle_command(const char *line) { (void)line; return false; }

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */

#endif /* SERIAL_FLASH_H */
