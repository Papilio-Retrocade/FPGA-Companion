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
#include "driver/usb_serial_jtag.h"
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

/* RX ring buffer for the low-level usb_serial_jtag driver used during the
 * raw payload phase (see serial_flash_io_begin/end below). Sized well above
 * the 64-byte USB-FS packet size so the driver's ISR can absorb bursts
 * between our read_raw_bytes() calls without stalling the host. */
#define SERIAL_FLASH_DRIVER_RX_BUF 16384
#define SERIAL_FLASH_DRIVER_TX_BUF 256

/* Console's default RX line-ending mode (CONFIG_LIBC_STDIN_LINE_ENDING_CR in
 * sdkconfig.release converts every raw 0x0D byte to 0x0A). Must be restored
 * after reading the raw bitstream or the text-line commands in
 * wifi_provision.c stop parsing correctly. */
#define CONSOLE_DEFAULT_RX_LINE_ENDINGS ESP_LINE_ENDINGS_CR

/* Whether the low-level usb_serial_jtag driver is currently installed for
 * the raw payload phase (see serial_flash_io_begin/end). When false,
 * read_raw_bytes() falls back to the slow stdio path. */
static bool s_driver_installed = false;

/* A handful of bytes that got over-read into stdio's internal FILE* buffer
 * while wifi_provision.c's getchar()-based line reader parsed the
 * FPGA_FLASH_BEGIN command line, drained via serial_flash_io_begin() before
 * the low-level driver takes over the RX FIFO. */
static uint8_t s_prefix_buf[64];
static size_t  s_prefix_len = 0;
static size_t  s_prefix_off = 0;

/* Prepares for the raw payload transfer: drains whatever's already sitting
 * in stdio's buffer (never blocks - fread() returns 0 immediately if
 * nothing is buffered), then installs the low-level usb_serial_jtag driver
 * so read_raw_bytes() can use its ISR-fed ring buffer instead of the
 * default console VFS's single-shot, effectively-non-blocking FIFO peek
 * (which caps throughput at roughly one 64-byte USB packet per
 * SERIAL_FLASH_EOF_DELAY_MS, i.e. only a few KB/s). */
static void serial_flash_io_begin(void)
{
    s_prefix_len = 0;
    s_prefix_off = 0;
    size_t r;
    while (s_prefix_len < sizeof(s_prefix_buf) &&
           (r = fread(s_prefix_buf + s_prefix_len, 1, sizeof(s_prefix_buf) - s_prefix_len, stdin)) > 0) {
        s_prefix_len += r;
    }
    clearerr(stdin);

    usb_serial_jtag_driver_config_t drv_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    drv_cfg.rx_buffer_size = SERIAL_FLASH_DRIVER_RX_BUF;
    drv_cfg.tx_buffer_size = SERIAL_FLASH_DRIVER_TX_BUF;
    esp_err_t err = usb_serial_jtag_driver_install(&drv_cfg);
    if (err == ESP_OK) {
        s_driver_installed = true;
    } else {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install failed (%s), falling back to slow stdio reads", esp_err_to_name(err));
        s_driver_installed = false;
    }
}

/* Hands the RX FIFO back to the console's default VFS path so getchar()-based
 * text commands keep working after the transfer completes. */
static void serial_flash_io_end(void)
{
    if (s_driver_installed) {
        usb_serial_jtag_driver_uninstall();
        s_driver_installed = false;
    }
}

/* Blocks reading exactly `n` raw bytes. Serves the small stdio-buffered
 * prefix captured by serial_flash_io_begin() first, then reads the bulk of
 * the transfer via the low-level usb_serial_jtag driver's blocking
 * usb_serial_jtag_read_bytes(), which receives from an ISR-fed ring buffer
 * instead of the default console VFS's single-shot FIFO peek (that peek
 * returns EWOULDBLOCK immediately whenever the 64-byte hardware FIFO
 * happens to be momentarily empty, regardless of the fd's blocking mode -
 * see usb_serial_jtag_vfs.c - which drove our old fread()-based loop into
 * its stall-retry delay almost every call and capped throughput at only a
 * few KB/s). Falls back to fread() on stdin if the driver failed to
 * install. Gives up after SERIAL_FLASH_STALL_MS of no data (e.g. host
 * disappeared mid-transfer) rather than hanging forever with flash/JTAG
 * state half-open. */
static bool read_raw_bytes(uint8_t *buf, size_t n)
{
    size_t got = 0;

    if (s_prefix_off < s_prefix_len) {
        size_t avail = s_prefix_len - s_prefix_off;
        size_t take  = avail < n ? avail : n;
        memcpy(buf, s_prefix_buf + s_prefix_off, take);
        s_prefix_off += take;
        got = take;
    }

    int stalled_ms = 0;
    while (got < n) {
        size_t r;
        if (s_driver_installed) {
            r = (size_t)usb_serial_jtag_read_bytes(buf + got, n - got, pdMS_TO_TICKS(SERIAL_FLASH_EOF_DELAY_MS));
        } else {
            r = fread(buf + got, 1, n - got, stdin);
            if (r == 0) clearerr(stdin);
        }
        if (r > 0) {
            got += r;
            stalled_ms = 0;
        } else {
            if (!s_driver_installed) vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASH_EOF_DELAY_MS));
            stalled_ms += SERIAL_FLASH_EOF_DELAY_MS;
            if (stalled_ms >= SERIAL_FLASH_STALL_MS) return false;
        }
    }
    return true;
}

/* =========================================================================
 * target=flash — stream bytes to SPI flash @ OTA_FPGA_FLASH_ADDR
 * Mirrors handle_fpga_update() in ota_server.c.
 * ========================================================================= */

/* Loads the SRAM bootloader, brings up the SPI bridge, and erases the target
 * flash region. Run this to completion *before* telling the host READY -
 * the bootloader load + init + 2MB erase together can take 10s of seconds,
 * and the host starts streaming the whole payload the instant it sees
 * READY. The USB-Serial/JTAG driver's RX ring buffer is only
 * SERIAL_FLASH_DRIVER_RX_BUF bytes, so if nothing drains it for that long
 * the rest of the payload stalls at the USB transport layer and the
 * eventual read_raw_bytes() calls in the write loop time out. Doing the
 * slow prep work up front means the write loop can start draining bytes
 * immediately once streaming begins. */
static esp_err_t serial_flash_prepare_spi(void)
{
    bt_hid_set_scan_paused(true);
    wifi_log_led_set(32, 0, 32);  /* purple = flash in progress */

    esp_err_t err = ota_server_load_bootloader_to_sram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootloader SRAM load failed: %s", esp_err_to_name(err));
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
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        return err;
    }

    mcu_hw_spi_flash_begin();

    ESP_LOGI(TAG, "Erasing flash region @ 0x%06x (%d bytes)", OTA_FPGA_FLASH_ADDR, OTA_FPGA_FLASH_SIZE);
    mcu_hw_erase_flash_region(OTA_FPGA_FLASH_ADDR, OTA_FPGA_FLASH_SIZE);

    return ESP_OK;
}

/* Streams `size` bytes to SPI flash. Assumes serial_flash_prepare_spi()
 * already ran successfully (bootloader in SRAM, SPI bridge up, region
 * erased) - mirrors handle_fpga_update() in ota_server.c from that point on. */
static esp_err_t serial_flash_write_spi(size_t size)
{
    ESP_LOGI(TAG, "Serial FPGA flash: %zu bytes -> flash @ 0x%06x", size, OTA_FPGA_FLASH_ADDR);

    uint8_t *buf = malloc(SERIAL_FLASH_CHUNK);
    if (!buf) {
        mcu_hw_spi_flash_end();
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        return ESP_ERR_NO_MEM;
    }

    uint32_t addr      = OTA_FPGA_FLASH_ADDR;
    size_t   remaining = size;
    size_t   written   = 0;

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

        /* ACK every chunk (not just every 65536 bytes) so the host can use
         * this as a per-chunk flow-control signal: it waits for this line
         * before sending the next chunk, bounding how much unread data can
         * ever sit in the usb_serial_jtag driver's small RX ring buffer.
         * Without this, the host streams the whole payload as fast as the
         * OS/USB stack will accept it, which can overrun the ring buffer
         * and wedge the USB transport (permanent stall, no recovery) if
         * mcu_hw_write_flash() is ever slower than the incoming data rate. */
        printf("PROGRESS %zu\r\n", written);
        fflush(stdout);
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
        printf("\r\nFPGA_FLASH_ERROR bad_command\r\n");
        fflush(stdout);
        return true;
    }

    bool is_flash = (strcmp(target, "flash") == 0);
    bool is_sram  = (strcmp(target, "sram") == 0);
    if (!is_flash && !is_sram) {
        printf("\r\nFPGA_FLASH_ERROR bad_target\r\n");
        fflush(stdout);
        return true;
    }

    size_t size = (size_t)size_ul;
    if (size == 0 || size > SERIAL_FLASH_MAX_SIZE) {
        printf("\r\nFPGA_FLASH_ERROR bad_size\r\n");
        fflush(stdout);
        return true;
    }

    /* For target=flash, do the slow prep (bootloader SRAM load + SPI bridge
     * init + full-region erase) *before* saying READY, not after - see
     * serial_flash_prepare_spi()'s comment for why. target=sram has no
     * erase step and stays as-is. */
    if (is_flash) {
        esp_err_t prep_err = serial_flash_prepare_spi();
        if (prep_err != ESP_OK) {
            printf("\r\nFPGA_FLASH_ERROR %s\r\n", esp_err_to_name(prep_err));
            fflush(stdout);
            return true;
        }
    }

    /* Leading \r\n guards against a leftover unterminated line still sitting
     * in the USB-Serial/JTAG TX FIFO (e.g. an early boot log fragment that
     * was queued before the host's connection was open) merging with this
     * response and breaking the host's strict ^READY$ line match. */
    printf("\r\nREADY\r\n");
    fflush(stdout);

    /* Raw bitstream bytes may legitimately contain 0x0D - stop the console
     * VFS translating it to 0x0A for the duration of the transfer. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    serial_flash_io_begin();

    esp_err_t err = is_flash ? serial_flash_write_spi(size) : serial_flash_write_sram(size);

    serial_flash_io_end();

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
