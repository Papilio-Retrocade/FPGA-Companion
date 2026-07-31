#ifndef OTA_SERVER_H
#define OTA_SERVER_H

/*
 * ota_server.h - HTTP OTA firmware update server for FPGA Companion
 *
 * Starts a lightweight HTTP server that accepts firmware uploads via POST,
 * writes them to the inactive OTA partition, and reboots into the new image.
 *
 * Requires WIFI_LOG_ENABLE (WiFi connection infrastructure) and a partition
 * table with two OTA app slots (see partitions_ota.csv).
 *
 * Usage after flashing:
 *   curl -X POST http://<device-ip>:3232/update --data-binary @build/fpga_companion.bin
 *
 * Status / current version:
 *   curl http://<device-ip>:3232/
 *
 * The device IP is printed to the log when WiFi connects.
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_OTA_ENABLE)

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Start the OTA HTTP server. Call this after WiFi is connected.
 * The server listens on CONFIG_OTA_PORT (default 3232).
 */
void ota_server_start(void);

/* FPGA bitstream flash region — shared with serial_flash.c so the USB-serial
 * fallback erases/writes the exact same address range as the HTTP OTA path. */
#define OTA_FPGA_FLASH_ADDR 0x000000
#define OTA_FPGA_FLASH_SIZE 0x200000

/**
 * Load the embedded bootloader to FPGA SRAM via JTAG so the SPI flash pins
 * are bridged to the ESP32. Shared by the HTTP OTA handlers and the serial
 * flash fallback (serial_flash.c) — safe to call even if ota_server_start()
 * has never run (lazily creates the JTAG mutex on first use).
 */
esp_err_t ota_server_load_bootloader_to_sram(void);

/**
 * Acquire/release the JTAG mutex shared by every JTAG-programming path
 * (HTTP OTA handlers and serial_flash.c), so a serial-triggered SRAM/flash
 * program can't race an HTTP-triggered one. Lazily creates the mutex.
 *
 * @param timeout_ms  How long to wait for the mutex.
 * @return true if the mutex was acquired.
 */
bool ota_server_jtag_lock(uint32_t timeout_ms);
void ota_server_jtag_unlock(void);

/** Ensure jtag_gowin_init() has run. Idempotent — no-op after first success. */
esp_err_t ota_server_jtag_ensure_init(void);

#else

static inline void ota_server_start(void) {}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */

#endif /* OTA_SERVER_H */
