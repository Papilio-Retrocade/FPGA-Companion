/*
 * jtag_gowin.h - Minimal JTAG programmer for Gowin GW2A FPGAs
 *
 * Directly programs .fs bitstream files to FPGA SRAM via JTAG.
 * No SVF conversion required - works with native Gowin bitstreams.
 *
 * Supports:
 *  - SRAM configuration (volatile, instant)
 *  - IDCODE verification
 *  - Device detection
 *
 * Pin connections (from schematic):
 *  ESP32 GPIO5 → FPGA TDO (C6)
 *  ESP32 GPIO6 → FPGA TCK (A7)
 *  ESP32 GPIO7 → FPGA TDI (A6)
 *  ESP32 GPIO8 → FPGA TMS (B8)
 */

#ifndef JTAG_GOWIN_H
#define JTAG_GOWIN_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

/* ========================================================================= */
/* JTAG Pin Configuration                                                    */
/* ========================================================================= */

typedef struct {
    gpio_num_t tdi;  // Test Data In  (ESP32 → FPGA)
    gpio_num_t tdo;  // Test Data Out (FPGA → ESP32)
    gpio_num_t tck;  // Test Clock    (ESP32 → FPGA)
    gpio_num_t tms;  // Test Mode Select (ESP32 → FPGA)
} jtag_pins_t;

/* Default pin configuration for Papilio RetroCade */
#define JTAG_DEFAULT_PINS { \
    .tdi = GPIO_NUM_7,  /* GPIO7 → TDI (A6) */ \
    .tdo = GPIO_NUM_5,  /* GPIO5 → TDO (C6) */ \
    .tck = GPIO_NUM_6,  /* GPIO6 → TCK (A7) */ \
    .tms = GPIO_NUM_8   /* GPIO8 → TMS (B8) */ \
}

/* ========================================================================= */
/* Gowin Device IDs                                                          */
/* ========================================================================= */

#define GOWIN_IDCODE_GW2A_18    0x0900281B  /* GW2A-LV18PG256C8/I7 */
#define GOWIN_IDCODE_GW2AR_18   0x0100481B  /* GW2AR-LV18QN88C8/I7 */
#define GOWIN_IDCODE_GW1N_1     0x0900181B  /* GW1N-LV1QN48C6/I5 */
#define GOWIN_IDCODE_GW1NR_9    0x0100181B  /* GW1NR-LV9QN88PC6/I5 */

/* ========================================================================= */
/* API Functions                                                             */
/* ========================================================================= */

/**
 * Initialize JTAG interface
 * 
 * @param pins  Pin configuration (or NULL for default)
 * @return ESP_OK on success
 */
esp_err_t jtag_gowin_init(const jtag_pins_t *pins);

/**
 * Read IDCODE from FPGA via JTAG
 * 
 * @param idcode  Pointer to store 32-bit IDCODE
 * @return ESP_OK on success
 */
esp_err_t jtag_gowin_read_idcode(uint32_t *idcode);

/**
 * Program FPGA SRAM with bitstream (.fs file)
 * 
 * This is volatile - configuration is lost on power cycle.
 * Use for development or bootstrapping.
 * 
 * @param bitstream  Raw .fs bitstream data
 * @param length     Bitstream length in bytes
 * @return ESP_OK on success
 * 
 * Example:
 *   uint8_t *bitstream = ... // Received via HTTP
 *   size_t len = ... 
 *   jtag_gowin_program_sram(bitstream, len);
 */
esp_err_t jtag_gowin_program_sram(const uint8_t *bitstream, size_t length);

/**
 * Verify FPGA is running and responding
 * 
 * @return ESP_OK if FPGA responds correctly
 */
esp_err_t jtag_gowin_verify(void);

/**
 * Get device name from IDCODE
 * 
 * @param idcode  Device IDCODE
 * @return Device name string (or "Unknown")
 */
const char* jtag_gowin_device_name(uint32_t idcode);

/* ========================================================================= */
/* Streaming API - For Large Bitstreams                                     */
/* ========================================================================= */

/**
 * Begin streaming SRAM programming
 * 
 * Initializes TAP, verifies device, and prepares for data streaming.
 * Must be followed by one or more calls to jtag_gowin_program_sram_write(),
 * then jtag_gowin_program_sram_end().
 * 
 * @param idcode_out  Optional pointer to receive device IDCODE
 * @return ESP_OK on success
 * 
 * Example:
 *   uint32_t idcode;
 *   jtag_gowin_program_sram_begin(&idcode);
 *   while (more_data) {
 *       jtag_gowin_program_sram_write(chunk, chunk_len);
 *   }
 *   jtag_gowin_program_sram_end();
 */
esp_err_t jtag_gowin_program_sram_begin(uint32_t *idcode_out);

/**
 * Stream bitstream chunk to FPGA SRAM
 * 
 * Must be called between jtag_gowin_program_sram_begin() and 
 * jtag_gowin_program_sram_end().
 * 
 * @param data    Bitstream chunk data
 * @param length  Chunk length in bytes
 * @return ESP_OK on success
 */
esp_err_t jtag_gowin_program_sram_write(const uint8_t *data, size_t length);

/**
 * Complete streaming SRAM programming
 * 
 * Finalizes programming and starts FPGA configuration.
 * Must be called after jtag_gowin_program_sram_begin() and 
 * jtag_gowin_program_sram_write().
 * 
 * @return ESP_OK on success
 */
esp_err_t jtag_gowin_program_sram_end(void);

#endif /* JTAG_GOWIN_H */
