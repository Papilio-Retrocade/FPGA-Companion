/*
 * ota_server.c - HTTP OTA firmware update server for FPGA Companion
 *
 * Hosts a minimal HTTP server on port CONFIG_OTA_PORT (default 3232).
 *
 *   GET  /        — plain-text status: running partition, firmware version, upload instructions
 *   POST /update  — upload raw firmware binary; writes to inactive OTA slot, then reboots
 *
 * Upload example:
 *   curl -X POST http://<device-ip>:3232/update --data-binary @build/fpga_companion.bin
 *
 * The device IP is logged when WiFi connects (search the log for "got ip:").
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_OTA_ENABLE)

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

#include "miniz.h"

#include "ota_server.h"
#include "../mcu_hw.h"
#include "jtag_gowin.h"
#include "bt_hid.h"
#include "wifi_log.h"
#include "../sysctrl.h"
#include "bootloader_data.h"

static const char *TAG = "ota_server";

/* =========================================================================
 * CORS / Private Network Access support
 *
 * Lets a browser-hosted page (e.g. the hosted web flasher at
 * papilioworks.com/flash) call these endpoints directly via fetch() from a
 * different origin. Chrome additionally requires the target to answer the
 * OPTIONS preflight with Access-Control-Allow-Private-Network before it will
 * let a public HTTPS page talk to a device on the local network.
 * ========================================================================= */

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", CONFIG_OTA_CORS_ALLOW_ORIGIN);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Content-Length");
}

/* Wrap every response call with add_cors_headers() without touching each of
 * the call sites below. Real functions are captured here before the macros
 * make httpd_resp_send_err/httpd_resp_sendstr expand to these wrappers for
 * the rest of this file. */
static esp_err_t ota_send_err_impl(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    add_cors_headers(req);
    return httpd_resp_send_err(req, error, msg);
}

static esp_err_t ota_send_str_impl(httpd_req_t *req, const char *msg)
{
    add_cors_headers(req);
    return httpd_resp_sendstr(req, msg);
}

#define httpd_resp_send_err(req, error, msg) ota_send_err_impl(req, error, msg)
#define httpd_resp_sendstr(req, msg)         ota_send_str_impl(req, msg)

/* Generic OPTIONS preflight handler, shared by every POST endpoint below. */
static esp_err_t handle_options_preflight(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Mutex to prevent concurrent JTAG programming requests */
static SemaphoreHandle_t s_jtag_mutex = NULL;

/* Shared JTAG initialisation flag (once per boot) */
static bool s_jtag_initialized = false;

/* Pre-allocated decompression buffers for bootloader SRAM loading.
 * Placed in BSS so they're guaranteed available regardless of heap state.
 * Ring buffer must be 32 KB (DEFLATE max look-back window).  Reused on
 * every OTA — avoids fragmenting the heap with repeated 32 KB allocations
 * that fail after the system has been running for a while. */
#define BOOTLOADER_RING_SIZE 32768
static uint8_t            s_bootloader_ring[BOOTLOADER_RING_SIZE];
static tinfl_decompressor s_bootloader_decomp;

/* Receive buffer size (bytes). Larger = faster upload but more heap. */
#define OTA_RECV_BUF 4096
#define FPGA_FLASH_BUF 8192
#define FPGA_FLASH_ADDR 0x000000  /* FPGA bitstream location in SPI flash */
#define FPGA_FLASH_SIZE 0x200000  /* 2 MB max for FPGA bitstream */
#define ROM_FLASH_MIN_ADDR 0x200000 /* Minimum safe address for /flash-write (protects bitstream) */
#define ROM_FLASH_MAX_SIZE 0x8000   /* 32 KB max per ROM write */

/* JTAG Programming Task - runs in separate task to avoid blocking HTTP server */
typedef struct {
    uint8_t *bitstream;
    size_t length;
} jtag_program_task_data_t;

static void jtag_program_task(void *arg)
{
    jtag_program_task_data_t *data = (jtag_program_task_data_t *)arg;
    
    ESP_LOGI(TAG, "JTAG programming task started: %zu bytes", data->length);
    
    /* Initialize JTAG */
    if (!s_jtag_initialized) {
        esp_err_t err = jtag_gowin_init(NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG initialization failed");
            free(data->bitstream);
            free(data);
            vTaskDelete(NULL);
            return;
        }
        s_jtag_initialized = true;
    }
    
    /* Begin streaming SRAM programming */
    uint32_t idcode;
    esp_err_t err = jtag_gowin_program_sram_begin(&idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin JTAG programming");
        free(data->bitstream);
        free(data);
        vTaskDelete(NULL);
        return;
    }
    
    /* Program in chunks */
    size_t chunk_size = 4096;
    for (size_t offset = 0; offset < data->length; offset += chunk_size) {
        size_t remaining = data->length - offset;
        size_t to_write = (remaining < chunk_size) ? remaining : chunk_size;
        
        err = jtag_gowin_program_sram_write(data->bitstream + offset, to_write, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG write failed at offset %zu", offset);
            free(data->bitstream);
            free(data);
            vTaskDelete(NULL);
            return;
        }
        
        if ((offset % 65536) == 0) {
            ESP_LOGI(TAG, "Programmed: %zu / %zu bytes", offset + to_write, data->length);
        }
    }
    
    /* Complete programming */
    err = jtag_gowin_program_sram_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG programming end failed");
    } else {
        ESP_LOGI(TAG, "JTAG programming completed successfully");
    }
    
    free(data->bitstream);
    free(data);
    vTaskDelete(NULL);
}

/* =========================================================================
 * GET / — status page
 * ========================================================================= */

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *app     = esp_app_get_description();

    char buf[1024];
    int  n = snprintf(buf, sizeof(buf),
        "FPGA Companion OTA Server\r\n"
        "=========================\r\n"
        "Running partition : %s\r\n"
        "Firmware version  : %s\r\n"
        "Build date        : %s %s\r\n"
        "\r\n"
        "Upload commands:\r\n"
        "  ESP32 firmware (dual-partition OTA):\r\n"
        "    curl -X POST http://<device-ip>:%d/update --data-binary @build/fpga_companion.bin\r\n"
        "\r\n"
        "  FPGA bitstream to flash (persistent, slower):\r\n"
        "    curl -X POST http://<device-ip>:%d/fpga-update --data-binary @bitstream.bin\r\n"
        "    NOTE: embedded bootloader is auto-loaded to FPGA SRAM via JTAG first.\r\n"
        "\r\n"
        "  FPGA bitstream to SRAM via JTAG (volatile, fast, no flash wear):\r\n"
        "    curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @bitstream.fs\r\n"
        "\r\n"
        "  Write ROM/data to SPI flash at arbitrary offset (addr >= 0x200000):\r\n"
        "    curl -X POST \"http://<device-ip>:%d/flash-write?addr=0x200000\" --data-binary @2dosa_c.bin\r\n",
        running ? running->label : "unknown",
        app->version,
        app->date, app->time,
        CONFIG_OTA_PORT,
        CONFIG_OTA_PORT,
        CONFIG_OTA_PORT,
        CONFIG_OTA_PORT);

    add_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* =========================================================================
 * POST /update — receive binary, write to inactive OTA partition, reboot
 * ========================================================================= */

static esp_err_t handle_update(httpd_req_t *req)
{
    esp_err_t err;

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA update partition found — check partition table");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition. Flash with partitions_ota.csv.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update started: %d bytes → partition '%s'",
             req->content_len, update_part->label);

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_RECV_BUF);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining  = req->content_len;
    int written    = 0;
    bool hdr_ok    = false;

    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf,
                                  remaining < OTA_RECV_BUF ? remaining : OTA_RECV_BUF);
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        /* Validate image magic on first chunk */
        if (!hdr_ok) {
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid image magic 0x%02x (expected 0xE9)", (uint8_t)buf[0]);
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Not a valid ESP32 firmware binary");
                return ESP_FAIL;
            }
            hdr_ok = true;
        }

        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }

        remaining -= recv;
        written   += recv;
        ESP_LOGD(TAG, "OTA progress: %d / %d bytes", written, req->content_len);
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Image validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update complete (%d bytes). Rebooting in 3 s ...", written);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OTA update successful. Rebooting in 3 seconds...\r\n");

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* =========================================================================
 * load_bootloader_to_sram — decompress and stream embedded bootloader to FPGA
 *
 * Called automatically by handle_fpga_update before writing to SPI flash.
 * This guarantees the FPGA is running a known-good bitstream (the bootloader)
 * that bridges the SPI flash pins to the ESP32, even when the flash is blank
 * or holds a corrupt/incompatible bitstream.
 *
 * Memory: uses only a 32 KB ring-buffer for decompression — no large malloc.
 * The compressed bitstream lives in ESP32 flash (const, zero RAM overhead).
 * ========================================================================= */

static esp_err_t load_bootloader_to_sram(void)
{
    if (xSemaphoreTake(s_jtag_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout acquiring JTAG mutex for bootloader load");
        return ESP_ERR_TIMEOUT;
    }

    /* NOTE: callers are responsible for pausing BLE scanning.  Don't toggle
     * the pause flag here — on success this function used to clear it,
     * which un-paused scanning in the middle of an OTA flash write and
     * caused TCP stalls. */

    if (!s_jtag_initialized) {
        esp_err_t err = jtag_gowin_init(NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG init failed: %s", esp_err_to_name(err));
            xSemaphoreGive(s_jtag_mutex);
            return err;
        }
        s_jtag_initialized = true;
    }

    ESP_LOGI(TAG, "Loading embedded bootloader (%u B compressed) to FPGA SRAM",
             BOOTLOADER_COMPRESSED_SIZE);

    /* Use pre-allocated static buffers (BSS).  No heap allocation needed —
     * this used to OOM after the heap got fragmented from running for a
     * while.  See declarations near the top of the file. */
    uint8_t            *ring   = s_bootloader_ring;
    tinfl_decompressor *decomp = &s_bootloader_decomp;
    const size_t        DICT_SIZE = BOOTLOADER_RING_SIZE;

    /* Retry loop — JTAG SRAM programming sometimes fails on the first
     * attempt when the SPI flash contains a corrupt user bitstream.  The
     * FPGA's config FSM keeps trying to auto-load the bad bitstream and its
     * partially-configured I/O cells fight the JTAG signals, producing a
     * spurious CRC error (STATUS=0x000000A1).
     *
     * Recovery: pulse RECONFIG_N low to abort the in-progress auto-load
     * (tri-states the user I/Os), then immediately drive JTAG CONFIG_ENABLE.
     * The TAP latches CONFIG_ENABLE before the config FSM begins its next
     * flash read, so SRAM programming wins the race and completes cleanly.
     *
     * Up to 3 attempts; each adds a slightly longer pre-JTAG settle. */
    const int MAX_ATTEMPTS = 3;
    esp_err_t write_err = ESP_FAIL;
    size_t    total_out = 0;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Bootloader load attempt %d/%d after failure (%s)",
                     attempt, MAX_ATTEMPTS, esp_err_to_name(write_err));
        }

        /* Brief RECONFIG_N pulse — aborts any in-progress flash auto-load
         * and tri-states user I/Os.  No post-delay; we race JTAG in next. */
        mcu_hw_fpga_reset_brief();
        /* Small settle so the config FSM enters its "ready for config" window
         * but well before the next flash auto-load would complete. */
        esp_rom_delay_us(2000 + attempt * 1000);  /* 3 ms, 4 ms, 5 ms */

        uint32_t  idcode;
        esp_err_t err = jtag_gowin_program_sram_begin(&idcode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG sram_begin failed: %s", esp_err_to_name(err));
            write_err = err;
            continue;
        }

        tinfl_init(decomp);
        const uint8_t *src      = bootloader_compressed_data;
        size_t         src_left = BOOTLOADER_COMPRESSED_SIZE;
        size_t         ring_pos = 0;
        total_out               = 0;
        write_err               = ESP_OK;

        for (;;) {
            size_t in_bytes  = src_left;
            size_t out_bytes = DICT_SIZE - ring_pos;

            tinfl_status status = tinfl_decompress(
                decomp,
                src, &in_bytes,
                ring,
                ring + ring_pos,
                &out_bytes,
                0);

            src      += in_bytes;
            src_left -= in_bytes;

            if (out_bytes > 0 && write_err == ESP_OK) {
                write_err  = jtag_gowin_program_sram_write(ring + ring_pos, out_bytes, false);
                total_out += out_bytes;
                ring_pos   = (ring_pos + out_bytes) % DICT_SIZE;
            }

            if (status == TINFL_STATUS_DONE) break;

            if (status < TINFL_STATUS_DONE) {
                ESP_LOGE(TAG, "Decompression error: %d", (int)status);
                if (write_err == ESP_OK) write_err = ESP_FAIL;
                break;
            }

            if (in_bytes == 0 && out_bytes == 0) {
                ESP_LOGE(TAG, "Decompressor stalled — corrupt data?");
                if (write_err == ESP_OK) write_err = ESP_FAIL;
                break;
            }
        }

        esp_err_t end_err = jtag_gowin_program_sram_end();
        if (end_err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG sram_end failed (attempt %d): %s",
                     attempt, esp_err_to_name(end_err));
            if (write_err == ESP_OK) write_err = end_err;
        }

        if (write_err == ESP_OK) break;  /* success — exit retry loop */
    }

    xSemaphoreGive(s_jtag_mutex);

    if (write_err == ESP_OK)
        ESP_LOGI(TAG, "Bootloader in FPGA SRAM (%zu bytes).  SPI flash now accessible.",
                 total_out);
    else
        ESP_LOGE(TAG, "Bootloader SRAM load failed after %d attempts (%s)",
                 MAX_ATTEMPTS, esp_err_to_name(write_err));

    return write_err;
}

/* =========================================================================
 * POST /fpga-update — receive FPGA bitstream, write to flash @ 0x000000
 * ========================================================================= */

static esp_err_t handle_fpga_update(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (req->content_len > FPGA_FLASH_SIZE) {
        ESP_LOGE(TAG, "FPGA bitstream too large: %d bytes (max %d)",
                 req->content_len, FPGA_FLASH_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bitstream too large");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA OTA update started: %d bytes → flash @ 0x%06x",
             req->content_len, FPGA_FLASH_ADDR);

    /* Pause BLE scanning for the duration of the flash operation.
     * BLE and SPI flash share radio/CPU time; scanning during flash writes
     * causes SPI timing issues and can corrupt the bitstream. */
    bt_hid_set_scan_paused(true);
    wifi_log_led_set(32, 0, 32);  /* purple = OTA in progress */

    /* Start timing */
    int64_t time_start = esp_timer_get_time();
    int64_t time_reset, time_erase;

    char *buf = malloc(FPGA_FLASH_BUF);
    if (!buf) {
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* Load the embedded bootloader to FPGA SRAM via JTAG.
     * This guarantees the FPGA is running (bridging SPI pins to the ESP32)
     * even when the flash is blank or holds a corrupt bitstream.
     * If the FPGA was already running a valid bitstream, JTAG programming
     * simply replaces it with the known-good bootloader. */
    ESP_LOGI(TAG, "Pre-loading embedded bootloader to FPGA SRAM");
    esp_err_t bl_err = load_bootloader_to_sram();
    if (bl_err != ESP_OK) {
        ESP_LOGE(TAG, "Bootloader SRAM load failed (%s) — aborting flash update",
                 esp_err_to_name(bl_err));
        free(buf);
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to load bootloader to FPGA SRAM");
        return ESP_FAIL;
    }

    /* Suppress MCU auto-reset while the bootloader is in SRAM.
     * The FPGA cold-boot event fired by JTAG programming must not trigger an
     * MCU reboot that would cause the FPGA to reload from flash mid-write. */
    sys_set_suppress_reset(true);

    /* Settle time for the FPGA to finish initialising from SRAM.
     * The simplified bootloader only bridges SPI pins — give it time to
     * stabilise before driving the SPI flash bus.  After a cold power-cycle
     * the FPGA boots from flash into the user bitstream (no SPI bridge);
     * JTAG-loading the bootloader is a hot swap and the SPI passthrough
     * needs a longer settle than after a warm reboot (where the bootloader
     * was already running). */
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Re-initialise the flash driver.  The very first call at boot fails because
     * the FPGA has no bitstream (SPI pins not bridged).  Now the SRAM bootloader
     * is running and the SPI bridge is active, so we can probe the chip. */
    ESP_LOGI(TAG, "Re-initializing SPI flash (FPGA SPI bridge now active)");
    esp_err_t flash_init_err = mcu_hw_reinit_flash();
    if (flash_init_err != ESP_OK) {
        ESP_LOGE(TAG, "SPI flash init failed after bootloader load: %s", esp_err_to_name(flash_init_err));
        mcu_hw_spi_flash_end();
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "SPI flash not accessible after SRAM bootloader load");
        sys_set_suppress_reset(false);
        return ESP_FAIL;
    }

    /* Acquire SPI flash semaphore for exclusive access */
    ESP_LOGI(TAG, "Acquiring SPI flash for programming");
    mcu_hw_spi_flash_begin();
    time_reset = esp_timer_get_time();

    /* Erase flash region */
    ESP_LOGI(TAG, "Erasing flash region @ 0x%06x (%d bytes)",
             FPGA_FLASH_ADDR, FPGA_FLASH_SIZE);
    mcu_hw_erase_flash_region(FPGA_FLASH_ADDR, FPGA_FLASH_SIZE);
    time_erase = esp_timer_get_time();
    ESP_LOGI(TAG, "Erase complete - took %.1f seconds", (time_erase - time_reset) / 1000000.0);

    /* Receive and write bitstream */
    ESP_LOGI(TAG, "Writing FPGA bitstream");
    int remaining = req->content_len;
    int written   = 0;
    uint32_t addr = FPGA_FLASH_ADDR;
    int64_t total_network_time = 0;
    int64_t total_write_time = 0;
    int chunk_count = 0;
    int timeout_streak = 0;

    while (remaining > 0) {
        chunk_count++;
        bool verbose = (chunk_count <= 8);  /* log first 8 chunks for diagnostics */

        if (verbose) ESP_LOGI(TAG, "chunk %d: recv start (remaining=%d)", chunk_count, remaining);
        int64_t t1 = esp_timer_get_time();
        int recv = httpd_req_recv(req, buf,
                                  remaining < FPGA_FLASH_BUF ? remaining : FPGA_FLASH_BUF);
        int64_t t2 = esp_timer_get_time();
        total_network_time += (t2 - t1);
        if (verbose) ESP_LOGI(TAG, "chunk %d: recv=%d in %.1f ms", chunk_count, recv, (t2 - t1) / 1000.0);

        if (recv == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Give Wi-Fi RX a chance to drain its queue.  Flash writes are
             * CPU-intensive and can starve the lwIP/Wi-Fi stack on the same
             * core.  After 3 consecutive timeouts the connection is dead. */
            timeout_streak++;
            ESP_LOGW(TAG, "chunk %d: recv timeout #%d", chunk_count, timeout_streak);
            vTaskDelay(pdMS_TO_TICKS(50));
            if (timeout_streak >= 3) {
                ESP_LOGE(TAG, "Connection stalled after %d bytes — aborting", written);
                free(buf);
                mcu_hw_spi_flash_end();
                bt_hid_set_scan_paused(false);
                wifi_log_led_set(0, 16, 0);
                /* Pulse RECONFIG_N so the FPGA isn't left wedged with a
                 * half-erased flash + active SRAM bootloader.  This lets the
                 * next OTA attempt re-enter cleanly. */
                mcu_hw_fpga_reset();
                sys_set_suppress_reset(false);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload stalled");
                return ESP_FAIL;
            }
            continue;
        }
        timeout_streak = 0;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d) after %d bytes written", recv, written);
            free(buf);
            mcu_hw_spi_flash_end();
            bt_hid_set_scan_paused(false);
            wifi_log_led_set(0, 16, 0);
            mcu_hw_fpga_reset();
            sys_set_suppress_reset(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        if (verbose) ESP_LOGI(TAG, "chunk %d: write %d bytes @ 0x%06x", chunk_count, recv, (unsigned)addr);
        int64_t t3 = esp_timer_get_time();
        mcu_hw_write_flash(addr, (uint8_t*)buf, recv);
        int64_t t4 = esp_timer_get_time();
        total_write_time += (t4 - t3);
        if (verbose) ESP_LOGI(TAG, "chunk %d: write done in %.1f ms", chunk_count, (t4 - t3) / 1000.0);

        addr      += recv;
        remaining -= recv;
        written   += recv;

        /* Log progress every 64KB */
        if (written % 65536 == 0 || remaining == 0) {
            ESP_LOGI(TAG, "Progress: %d / %d bytes (%.1f%%)",
                     written, req->content_len,
                     100.0 * written / req->content_len);
        }
    }

    int64_t time_write = esp_timer_get_time();
    free(buf);
    mcu_hw_spi_flash_end();

    /* Let SPI flash settle after intensive write operations */
    ESP_LOGI(TAG, "Flash write complete, waiting for flash to settle");
    vTaskDelay(pdMS_TO_TICKS(500));  // 500ms delay for flash to settle

    int64_t time_total = time_write - time_start;
    
    ESP_LOGI(TAG, "FPGA bitstream written successfully (%d bytes)", written);
    ESP_LOGI(TAG, "=== FPGA OTA Timing Breakdown ===");
    ESP_LOGI(TAG, "  Reset/setup:   %.1f s", (time_reset - time_start) / 1000000.0);
    ESP_LOGI(TAG, "  Flash erase:   %.1f s", (time_erase - time_reset) / 1000000.0);
    ESP_LOGI(TAG, "  Network recv:  %.1f s", total_network_time / 1000000.0);
    ESP_LOGI(TAG, "  Flash write:   %.1f s", total_write_time / 1000000.0);
    ESP_LOGI(TAG, "  Other/cleanup: %.1f s", (time_total - total_network_time - total_write_time - (time_erase - time_start)) / 1000000.0);
    ESP_LOGI(TAG, "  TOTAL TIME:    %.1f s", time_total / 1000000.0);
    ESP_LOGI(TAG, "  Throughput:    %.1f KB/s", (written / 1024.0) / (time_total / 1000000.0));
    ESP_LOGI(TAG, "=================================");
    
    /* Trigger FPGA reconfiguration via RECONFIG_N.
     * IMPORTANT: keep suppress_reset=true during the pulse.  The FPGA cold-boot
     * event fires immediately when RECONFIG_N goes low — if suppress is already
     * cleared the ESP32 will reboot itself before the HTTP response is sent.
     * We clear suppress_reset after a delay long enough for the FPGA to finish
     * booting directly from the new bitstream @ 0x000000. */
    ESP_LOGI(TAG, "Triggering FPGA reconfiguration via RECONFIG_N");
    bt_hid_set_scan_paused(false);
    wifi_log_led_set(0, 16, 0);
    mcu_hw_fpga_reset();

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "FPGA update successful! FPGA reconfiguring from new bitstream...\r\n");

    ESP_LOGI(TAG, "FPGA OTA update complete — waiting for FPGA boot sequence to finish");
    vTaskDelay(pdMS_TO_TICKS(2000));   /* margin for FPGA to boot directly from 0x000000 */
    sys_set_suppress_reset(false);
    ESP_LOGI(TAG, "Reset suppression cleared — FPGA should be running new bitstream");

    ESP_LOGI(TAG, "FPGA OTA update complete");
    return ESP_OK;
}

/* =========================================================================
 * POST /fpga-jtag-sram — program FPGA SRAM via JTAG (no flash needed)
 * ========================================================================= */

static esp_err_t handle_fpga_jtag_sram(httpd_req_t *req)
{
    esp_err_t err;

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    /* Reject concurrent programming requests */
    if (xSemaphoreTake(s_jtag_mutex, 0) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG programming already in progress");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming started: %d bytes", req->content_len);

    /* Initialize JTAG (if not already initialized) */
    if (!s_jtag_initialized) {
        err = jtag_gowin_init(NULL);  /* Use default pins */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG initialization failed");
            xSemaphoreGive(s_jtag_mutex);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG init failed");
            return ESP_FAIL;
        }
        s_jtag_initialized = true;
    }

    /* Pause BLE scanning for the duration — BLE and WiFi share the radio on
     * ESP32-S3; a 5-second scan starves WiFi and resets the TCP connection. */
    bt_hid_set_scan_paused(true);

    /* Begin streaming SRAM programming */
    uint32_t idcode;
    err = jtag_gowin_program_sram_begin(&idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin JTAG programming");
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                           "FPGA not detected via JTAG");
        return ESP_FAIL;
    }

    /* Allocate chunk buffer (4KB - much smaller than full bitstream) */
    char *chunk_buf = malloc(OTA_RECV_BUF);
    if (!chunk_buf) {
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* Stream bitstream in chunks directly to JTAG */
    int remaining = req->content_len;
    int received  = 0;
    bool error = false;

    while (remaining > 0 && !error) {
        int to_recv = (remaining < OTA_RECV_BUF) ? remaining : OTA_RECV_BUF;
        int recv = httpd_req_recv(req, chunk_buf, to_recv);
        
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(chunk_buf);
            xSemaphoreGive(s_jtag_mutex);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        
        /* Write chunk to FPGA via JTAG immediately */
        err = jtag_gowin_program_sram_write((uint8_t*)chunk_buf, recv, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG write failed");
            error = true;
            break;
        }
        
        remaining -= recv;
        received  += recv;

        /* Progress log every ~128KB (using last-logged threshold to handle
         * non-power-of-2 chunk totals). */
        static int last_logged = 0;
        if (received - last_logged >= 131072 || remaining == 0) {
            ESP_LOGI(TAG, "Programmed: %d / %d bytes", received, req->content_len);
            last_logged = received;
        }
        if (remaining == 0) {
            last_logged = 0;  /* reset for next transfer */
        }
    }

    free(chunk_buf);

    if (error) {
        bt_hid_set_scan_paused(false);
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    /* Complete SRAM programming */
    err = jtag_gowin_program_sram_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG programming end failed");
        bt_hid_set_scan_paused(false);
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming complete!");
    /* The newly loaded bitstream may not implement the MiSTeryNano SPI
     * protocol. Suppress the FPGA-cold-boot auto-reset so the SRAM design
     * stays running instead of being clobbered by an MCU reboot that would
     * trigger the FPGA to reload its SPI-flash bootloader bitstream. */
    sys_set_suppress_reset(true);
    bt_hid_set_scan_paused(false);
    xSemaphoreGive(s_jtag_mutex);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "FPGA programmed successfully via JTAG!\n");

    return ESP_OK;
}

/* =========================================================================
 * POST /flash-write?addr=0xNNNNNN — write raw binary to SPI flash at offset
 *
 * Writes data to SPI flash at the specified address.  Only addresses
 * >= 0x200000 are permitted; the FPGA bitstream region (0x000000-0x1FFFFF)
 * is protected — use /fpga-update for that region.
 *
 * Usage (flash Dolphin DOS 2 c1541 ROM):
 *   curl -X POST "http://<ip>:3232/flash-write?addr=0x200000" \
 *        --data-binary @2dosa_c.bin
 * ========================================================================= */

static esp_err_t handle_flash_write(httpd_req_t *req)
{
    /* Parse ?addr= query parameter */
    char query[64]    = {0};
    char addr_str[32] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "addr", addr_str, sizeof(addr_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing required query parameter: ?addr=0x200000");
        return ESP_FAIL;
    }

    char *endptr;
    uint32_t flash_addr = (uint32_t)strtoul(addr_str, &endptr, 0);
    if (endptr == addr_str) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid addr parameter");
        return ESP_FAIL;
    }

    if (flash_addr < ROM_FLASH_MIN_ADDR) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Address < 0x200000 is protected (FPGA bitstream region). Use /fpga-update.");
        return ESP_FAIL;
    }

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (req->content_len > ROM_FLASH_MAX_SIZE) {
        ESP_LOGE(TAG, "ROM too large: %d bytes (max %d)", req->content_len, ROM_FLASH_MAX_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data too large (max 32 KB per slot)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Flash write: %d bytes → 0x%06" PRIx32, req->content_len, flash_addr);

    bt_hid_set_scan_paused(true);
    wifi_log_led_set(32, 0, 32);  /* purple = flash write in progress */
    sys_set_suppress_reset(true);

    /* Load bootloader to FPGA SRAM so SPI pins are bridged to the ESP32 */
    esp_err_t err = load_bootloader_to_sram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootloader SRAM load failed: %s", esp_err_to_name(err));
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to load bootloader to FPGA SRAM");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    err = mcu_hw_reinit_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash reinit failed: %s", esp_err_to_name(err));
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPI flash not accessible");
        return ESP_FAIL;
    }

    mcu_hw_spi_flash_begin();

    /* Erase only the 4 KB sectors covered by this write */
    uint32_t erase_start = flash_addr & ~0xFFFu;
    uint32_t erase_end   = (flash_addr + req->content_len + 0xFFFu) & ~0xFFFu;
    uint32_t erase_size  = erase_end - erase_start;
    ESP_LOGI(TAG, "Erasing 0x%06" PRIx32 " .. 0x%06" PRIx32 " (%" PRIu32 " bytes)",
             erase_start, erase_end, erase_size);
    mcu_hw_erase_flash_region(erase_start, erase_size);

    /* Receive and write the ROM data */
    char *buf = malloc(FPGA_FLASH_BUF);
    if (!buf) {
        mcu_hw_spi_flash_end();
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 16, 0);
        sys_set_suppress_reset(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int      remaining = req->content_len;
    int      written   = 0;
    uint32_t addr      = flash_addr;

    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf,
                                  remaining < FPGA_FLASH_BUF ? remaining : FPGA_FLASH_BUF);
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(buf);
            mcu_hw_spi_flash_end();
            bt_hid_set_scan_paused(false);
            wifi_log_led_set(0, 16, 0);
            sys_set_suppress_reset(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        mcu_hw_write_flash(addr, (uint8_t *)buf, recv);
        addr      += recv;
        written   += recv;
        remaining -= recv;
    }

    free(buf);
    mcu_hw_spi_flash_end();

    ESP_LOGI(TAG, "Flash write complete: %d bytes @ 0x%06" PRIx32, written, flash_addr);

    /* Reconfigure FPGA from the bitstream in flash at 0x000000 */
    bt_hid_set_scan_paused(false);
    wifi_log_led_set(0, 16, 0);
    mcu_hw_fpga_reset();

    char resp[128];
    snprintf(resp, sizeof(resp),
             "Flash write successful! %d bytes @ 0x%06" PRIx32 ". FPGA reconfiguring...\r\n",
             written, flash_addr);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(2000));
    sys_set_suppress_reset(false);
    return ESP_OK;
}

/* =========================================================================
 * /fpga-recover — emergency recovery endpoint
 *
 * POST to this endpoint to:
 *   1. Load the embedded bootloader to FPGA SRAM via JTAG (works even if
 *      SPI flash is completely corrupt — JTAG bypasses flash entirely).
 *   2. Bulk-erase the FPGA bitstream region of SPI flash (0x000000 .. 0x100000).
 *
 * After this call the FPGA is running the bootloader (SPI bridge active) and
 * the flash bitstream area is blank.  You can then POST to /fpga-update to
 * write a fresh bitstream.
 *
 * Usage:
 *   curl -X POST http://<device-ip>:<port>/fpga-recover
 * ========================================================================= */
static esp_err_t handle_fpga_recover(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== FPGA Recovery requested ===");
    wifi_log_led_set(32, 0, 32);  /* purple = recovery in progress */
    bt_hid_set_scan_paused(true);

    /* Step 1: load bootloader to SRAM via JTAG */
    esp_err_t err = load_bootloader_to_sram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recovery: SRAM bootloader load failed: %s", esp_err_to_name(err));
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 32, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Recovery failed: could not load bootloader to SRAM via JTAG");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(300));  /* let FPGA SPI bridge come up */

    /* Step 2: reinit flash through the now-active SPI bridge */
    ESP_LOGI(TAG, "Recovery: re-initialising SPI flash");
    err = mcu_hw_reinit_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recovery: flash init failed: %s", esp_err_to_name(err));
        bt_hid_set_scan_paused(false);
        wifi_log_led_set(0, 32, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Recovery: SPI flash not accessible after SRAM load");
        return ESP_FAIL;
    }

    /* Step 3: bulk erase the bitstream region (first 1 MB) */
    ESP_LOGI(TAG, "Recovery: bulk erasing flash bitstream region (0x000000..0x100000)");
    mcu_hw_erase_flash_region(0x000000, 0x100000);
    ESP_LOGI(TAG, "Recovery: flash erase complete");

    bt_hid_set_scan_paused(false);
    wifi_log_led_set(0, 32, 0);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req,
        "Recovery complete.\n"
        "FPGA is running bootloader from SRAM.\n"
        "SPI flash bitstream region erased.\n"
        "POST to /fpga-update to write a fresh bitstream.\n");
    return ESP_OK;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void ota_server_start(void)
{
    if (s_jtag_mutex == NULL) {
        s_jtag_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = CONFIG_OTA_PORT;
    /* status, ESP32 update, FPGA flash update, FPGA JTAG update, FPGA recover,
     * flash-write, plus one OPTIONS preflight handler for each POST endpoint. */
    cfg.max_uri_handlers = 11;
    cfg.stack_size       = 8192;
    cfg.recv_wait_timeout = 15;   /* 15 s per recv — long stalls indicate a dead TCP connection,
                                     not a slow client.  Don't hang forever and leave flash half-erased. */
    cfg.send_wait_timeout = 15;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", CONFIG_OTA_PORT);
        return;
    }

    static const httpd_uri_t status_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_status,
    };
    httpd_register_uri_handler(server, &status_uri);

    static const httpd_uri_t update_uri = {
        .uri     = "/update",
        .method  = HTTP_POST,
        .handler = handle_update,
    };
    httpd_register_uri_handler(server, &update_uri);

    static const httpd_uri_t fpga_update_uri = {
        .uri     = "/fpga-update",
        .method  = HTTP_POST,
        .handler = handle_fpga_update,
    };
    httpd_register_uri_handler(server, &fpga_update_uri);

    static const httpd_uri_t fpga_jtag_sram_uri = {
        .uri     = "/fpga-jtag-sram",
        .method  = HTTP_POST,
        .handler = handle_fpga_jtag_sram,
    };
    httpd_register_uri_handler(server, &fpga_jtag_sram_uri);

    static const httpd_uri_t fpga_recover_uri = {
        .uri     = "/fpga-recover",
        .method  = HTTP_POST,
        .handler = handle_fpga_recover,
    };
    httpd_register_uri_handler(server, &fpga_recover_uri);

    static const httpd_uri_t flash_write_uri = {
        .uri     = "/flash-write",
        .method  = HTTP_POST,
        .handler = handle_flash_write,
    };
    httpd_register_uri_handler(server, &flash_write_uri);

    /* OPTIONS preflight for every POST endpoint (Chrome sends this before
     * each cross-origin POST when Private Network Access is in play). */
    static const char *cors_paths[] = {
        "/update", "/fpga-update", "/fpga-jtag-sram", "/fpga-recover", "/flash-write",
    };
    static httpd_uri_t options_uris[5];
    for (size_t i = 0; i < sizeof(cors_paths) / sizeof(cors_paths[0]); i++) {
        options_uris[i].uri     = cors_paths[i];
        options_uris[i].method  = HTTP_OPTIONS;
        options_uris[i].handler = handle_options_preflight;
        httpd_register_uri_handler(server, &options_uris[i]);
    }

    ESP_LOGI(TAG, "OTA server ready on port %d", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  CORS origin    : %s", CONFIG_OTA_CORS_ALLOW_ORIGIN);
    ESP_LOGI(TAG, "  Status         : curl http://<device-ip>:%d/", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  ESP32 upload   : curl -X POST http://<device-ip>:%d/update --data-binary @build/fpga_companion.bin",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA flash     : curl -X POST http://<device-ip>:%d/fpga-update --data-binary @your_bitstream.bin",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA JTAG SRAM : curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @your_bitstream.fs",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA recovery  : curl -X POST http://<device-ip>:%d/fpga-recover",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  ROM flash write : curl -X POST \"http://<device-ip>:%d/flash-write?addr=0x200000\" --data-binary @2dosa_c.bin",
             CONFIG_OTA_PORT);
}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */
