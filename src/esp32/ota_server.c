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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "ota_server.h"
#include "../mcu_hw.h"
#include "jtag_gowin.h"
#include "bt_hid.h"
#include "../sysctrl.h"

static const char *TAG = "ota_server";

/* Mutex to prevent concurrent JTAG programming requests */
static SemaphoreHandle_t s_jtag_mutex = NULL;

/* Receive buffer size (bytes). Larger = faster upload but more heap. */
#define OTA_RECV_BUF 4096
#define FPGA_FLASH_BUF 4096
#define FPGA_FLASH_ADDR 0x100000  /* FPGA bitstream location in SPI flash */
#define FPGA_FLASH_SIZE 0x200000  /* 2 MB max for FPGA bitstream */

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
    static bool jtag_initialized = false;
    if (!jtag_initialized) {
        esp_err_t err = jtag_gowin_init(NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG initialization failed");
            free(data->bitstream);
            free(data);
            vTaskDelete(NULL);
            return;
        }
        jtag_initialized = true;
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
        
        err = jtag_gowin_program_sram_write(data->bitstream + offset, to_write);
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

    char buf[768];
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
        "\r\n"
        "  FPGA bitstream to SRAM via JTAG (volatile, fast, no flash wear):\r\n"
        "    curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @bitstream.fs\r\n",
        running ? running->label : "unknown",
        app->version,
        app->date, app->time,
        CONFIG_OTA_PORT,
        CONFIG_OTA_PORT,
        CONFIG_OTA_PORT);

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
 * POST /fpga-update — receive FPGA bitstream, write to flash @ 0x100000
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

    /* Start timing */
    int64_t time_start = esp_timer_get_time();
    int64_t time_reset, time_erase;

    char *buf = malloc(FPGA_FLASH_BUF);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* NOTE: FPGA must NOT be held in reset during programming!
     * The FPGA needs to be running for the ESP32 to access the shared SPI flash.
     * When FPGA is in reset, SPI pins are disconnected from ESP32. */
    
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

    while (remaining > 0) {
        int64_t t1 = esp_timer_get_time();
        int recv = httpd_req_recv(req, buf,
                                  remaining < FPGA_FLASH_BUF ? remaining : FPGA_FLASH_BUF);
        int64_t t2 = esp_timer_get_time();
        total_network_time += (t2 - t1);
        
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(buf);
            mcu_hw_spi_flash_end();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        int64_t t3 = esp_timer_get_time();
        mcu_hw_write_flash(addr, (uint8_t*)buf, recv);
        int64_t t4 = esp_timer_get_time();
        total_write_time += (t4 - t3);

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
    
    /* Trigger FPGA reconfiguration by pulsing reset low then high */
    ESP_LOGI(TAG, "Triggering FPGA reconfiguration from new bitstream");
    gpio_set_direction(13, GPIO_MODE_OUTPUT);  // PIN_NUM_RECONFIG_N
    gpio_set_level(13, 0);  // Assert reset
    vTaskDelay(pdMS_TO_TICKS(100));  // Hold reset for 100ms
    gpio_set_level(13, 1);  // Release reset
    gpio_set_direction(13, GPIO_MODE_INPUT);  // Return to input mode
    
    /* Wait for FPGA to reconfigure from flash */
    ESP_LOGI(TAG, "Waiting for FPGA reconfiguration to complete");
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second for FPGA reconfiguration

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "FPGA update successful! FPGA reconfiguring from new bitstream...\r\n");

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
    static bool jtag_initialized = false;
    if (!jtag_initialized) {
        err = jtag_gowin_init(NULL);  /* Use default pins */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG initialization failed");
            xSemaphoreGive(s_jtag_mutex);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG init failed");
            return ESP_FAIL;
        }
        jtag_initialized = true;
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
        err = jtag_gowin_program_sram_write((uint8_t*)chunk_buf, recv);
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
 * Public API
 * ========================================================================= */

void ota_server_start(void)
{
    if (s_jtag_mutex == NULL) {
        s_jtag_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = CONFIG_OTA_PORT;
    cfg.max_uri_handlers = 4;  /* status, ESP32 update, FPGA flash update, FPGA JTAG update */
    cfg.stack_size       = 8192;
    cfg.recv_wait_timeout = 600;  /* 10 minutes - for large JTAG bitstreams */
    cfg.send_wait_timeout = 600;  /* 10 minutes */

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

    ESP_LOGI(TAG, "OTA server ready on port %d", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  Status         : curl http://<device-ip>:%d/", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  ESP32 upload   : curl -X POST http://<device-ip>:%d/update --data-binary @build/fpga_companion.bin",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA flash     : curl -X POST http://<device-ip>:%d/fpga-update --data-binary @your_bitstream.bin",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA JTAG SRAM : curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @your_bitstream.fs",
             CONFIG_OTA_PORT);
}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */
