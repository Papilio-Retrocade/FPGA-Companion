/*
 * serial_flash.c - FPGA bitstream flashing over USB serial (OTA-over-IP fallback)
 *
 * See serial_flash.h for the line protocol. Reuses the exact same erase/write
 * primitives (mcu_hw_*) and JTAG bootloader loader (ota_server_load_bootloader_to_sram)
 * as the HTTP OTA handlers in ota_server.c, just with a serial byte source
 * instead of httpd_req_recv().
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_OTA_ENABLE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "serial_flash.h"
#include "ota_server.h"
#include "jtag_gowin.h"
#include "bt_hid.h"
#include "wifi_log.h"
#include "../mcu_hw.h"
#include "../sysctrl.h"

static const char *TAG = "serial_flash";

#define SERIAL_FLASH_CHUNK        4096
#define SERIAL_FLASH_MAX_SIZE     OTA_FPGA_FLASH_SIZE  /* 2 MB, same cap as /fpga-update */
#define SERIAL_FLASH_STALL_MS     30000
#define SERIAL_FLASH_EOF_DELAY_MS 20

/* Console's default RX line-ending mode (CONFIG_LIBC_STDIN_LINE_ENDING_CR in
 * sdkconfig.release converts every raw 0x0D byte to 0x0A). Must be restored
 * after reading the raw bitstream or the text-line commands in
 * wifi_provision.c stop parsing correctly. */
#define CONSOLE_DEFAULT_RX_LINE_ENDINGS ESP_LINE_ENDINGS_CR

/* Blocks reading exactly `n` raw bytes from stdin, no line-ending translation
 * (caller must have already switched to ESP_LINE_ENDINGS_LF). getchar() can
 * return EOF when no byte is available yet on this non-blocking VFS - back
 * off briefly and retry, same pattern as wifi_provision.c's read_line().
 * Gives up after SERIAL_FLASH_STALL_MS of no data (e.g. host disappeared
 * mid-transfer) rather than hanging forever with flash/JTAG state half-open. */
static bool read_raw_bytes(uint8_t *buf, size_t n)
{
    int stalled_ms = 0;
    for (size_t i = 0; i < n; i++) {
        int c;
        for (;;) {
            c = getchar();
            if (c != EOF) break;
            vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASH_EOF_DELAY_MS));
            stalled_ms += SERIAL_FLASH_EOF_DELAY_MS;
            if (stalled_ms >= SERIAL_FLASH_STALL_MS) return false;
        }
        stalled_ms = 0;
        buf[i] = (uint8_t)c;
    }
    return true;
}

/* =========================================================================
 * target=flash — stream bytes to SPI flash @ OTA_FPGA_FLASH_ADDR
 * Mirrors handle_fpga_update() in ota_server.c.
 * ========================================================================= */

static esp_err_t serial_flash_write_spi(size_t size)
{
    ESP_LOGI(TAG, "Serial FPGA flash: %zu bytes -> flash @ 0x%06x", size, OTA_FPGA_FLASH_ADDR);

    bt_hid_set_scan_paused(true);
    wifi_log_led_set(32, 0, 32);  /* purple = flash in progress */

    uint8_t *buf = malloc(SERIAL_FLASH_CHUNK);
    if (!buf) {
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ota_server_load_bootloader_to_sram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootloader SRAM load failed: %s", esp_err_to_name(err));
        free(buf);
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        return err;
    }

    /* Suppress MCU auto-reset while the bootloader is in SRAM (see comment
     * in ota_server.c's handle_fpga_update — the JTAG-triggered cold-boot
     * event must not reboot the MCU mid-write). */
    sys_set_suppress_reset(true);
    vTaskDelay(pdMS_TO_TICKS(1500));

    err = mcu_hw_reinit_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI flash init failed: %s", esp_err_to_name(err));
        free(buf);
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        return err;
    }

    mcu_hw_spi_flash_begin();

    ESP_LOGI(TAG, "Erasing flash region @ 0x%06x (%d bytes)", OTA_FPGA_FLASH_ADDR, OTA_FPGA_FLASH_SIZE);
    mcu_hw_erase_flash_region(OTA_FPGA_FLASH_ADDR, OTA_FPGA_FLASH_SIZE);

    uint32_t addr      = OTA_FPGA_FLASH_ADDR;
    size_t   remaining = size;
    size_t   written   = 0;
    size_t   last_logged = 0;

    while (remaining > 0) {
        size_t to_read = remaining < SERIAL_FLASH_CHUNK ? remaining : SERIAL_FLASH_CHUNK;
        if (!read_raw_bytes(buf, to_read)) {
            ESP_LOGE(TAG, "Serial read stalled after %zu / %zu bytes", written, size);
            free(buf);
            mcu_hw_spi_flash_end();
            bt_hid_set_scan_paused(false);
            wifi_log_led_set(0, 16, 0);
            mcu_hw_fpga_reset();
            sys_set_suppress_reset(false);
            return ESP_ERR_TIMEOUT;
        }

        mcu_hw_write_flash(addr, buf, to_read);
        addr      += to_read;
        remaining -= to_read;
        written   += to_read;

        if (written - last_logged >= 65536 || remaining == 0) {
            printf("PROGRESS %zu\r\n", written);
            fflush(stdout);
            last_logged = written;
        }
    }

    free(buf);
    mcu_hw_spi_flash_end();
    ESP_LOGI(TAG, "Flash write complete, waiting for flash to settle");
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Serial FPGA flash write successful (%zu bytes). Reconfiguring FPGA.", written);
    bt_hid_set_scan_paused(false);
    wifi_log_led_set(0, 16, 0);
    mcu_hw_fpga_reset();

    vTaskDelay(pdMS_TO_TICKS(2000));  /* margin for FPGA to boot from new bitstream */
    sys_set_suppress_reset(false);
    return ESP_OK;
}

/* =========================================================================
 * target=sram — stream bytes to FPGA SRAM via JTAG (volatile)
 * Mirrors handle_fpga_jtag_sram() in ota_server.c.
 * ========================================================================= */

static esp_err_t serial_flash_write_sram(size_t size)
{
    ESP_LOGI(TAG, "Serial FPGA JTAG SRAM program: %zu bytes", size);

    if (!ota_server_jtag_lock(0)) {
        ESP_LOGW(TAG, "JTAG busy - another programming request in progress");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ota_server_jtag_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG init failed: %s", esp_err_to_name(err));
        ota_server_jtag_unlock();
        return err;
    }

    bt_hid_set_scan_paused(true);

    uint32_t idcode;
    err = jtag_gowin_program_sram_begin(&idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FPGA not detected via JTAG");
        bt_hid_set_scan_paused(false);
        ota_server_jtag_unlock();
        return err;
    }

    uint8_t *buf = malloc(SERIAL_FLASH_CHUNK);
    if (!buf) {
        jtag_gowin_program_sram_end();
        bt_hid_set_scan_paused(false);
        ota_server_jtag_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t    remaining   = size;
    size_t    received    = 0;
    size_t    last_logged = 0;
    esp_err_t write_err   = ESP_OK;

    while (remaining > 0) {
        size_t to_read = remaining < SERIAL_FLASH_CHUNK ? remaining : SERIAL_FLASH_CHUNK;
        if (!read_raw_bytes(buf, to_read)) {
            ESP_LOGE(TAG, "Serial read stalled after %zu / %zu bytes", received, size);
            write_err = ESP_ERR_TIMEOUT;
            break;
        }

        write_err = jtag_gowin_program_sram_write(buf, to_read, true);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG write failed at offset %zu", received);
            break;
        }

        remaining -= to_read;
        received  += to_read;

        if (received - last_logged >= 131072 || remaining == 0) {
            printf("PROGRESS %zu\r\n", received);
            fflush(stdout);
            last_logged = received;
        }
    }

    free(buf);

    esp_err_t end_err = jtag_gowin_program_sram_end();
    if (write_err == ESP_OK) write_err = end_err;

    if (write_err == ESP_OK) {
        ESP_LOGI(TAG, "Serial FPGA JTAG SRAM programming complete!");
        /* Bitstream may not implement the MiSTeryNano SPI protocol - suppress
         * the FPGA-cold-boot auto-reset (same as /fpga-jtag-sram). */
        sys_set_suppress_reset(true);
    }

    bt_hid_set_scan_paused(false);
    ota_server_jtag_unlock();
    return write_err;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

bool serial_flash_try_handle_command(const char *line)
{
    static const char prefix[] = "FPGA_FLASH_BEGIN ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;

    char          target[8] = {0};
    unsigned long size_ul   = 0;
    if (sscanf(line + sizeof(prefix) - 1, "%7s %lu", target, &size_ul) != 2) {
        printf("FPGA_FLASH_ERROR bad_command\r\n");
        fflush(stdout);
        return true;
    }

    bool is_flash = (strcmp(target, "flash") == 0);
    bool is_sram  = (strcmp(target, "sram") == 0);
    if (!is_flash && !is_sram) {
        printf("FPGA_FLASH_ERROR bad_target\r\n");
        fflush(stdout);
        return true;
    }

    size_t size = (size_t)size_ul;
    if (size == 0 || size > SERIAL_FLASH_MAX_SIZE) {
        printf("FPGA_FLASH_ERROR bad_size\r\n");
        fflush(stdout);
        return true;
    }

    printf("READY\r\n");
    fflush(stdout);

    /* Raw bitstream bytes may legitimately contain 0x0D - stop the console
     * VFS translating it to 0x0A for the duration of the transfer. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    esp_err_t err = is_flash ? serial_flash_write_spi(size) : serial_flash_write_sram(size);

    usb_serial_jtag_vfs_set_rx_line_endings(CONSOLE_DEFAULT_RX_LINE_ENDINGS);

    if (err == ESP_OK) {
        printf("FPGA_FLASH_OK\r\n");
    } else {
        printf("FPGA_FLASH_ERROR %s\r\n", esp_err_to_name(err));
    }
    fflush(stdout);
    return true;
}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */
