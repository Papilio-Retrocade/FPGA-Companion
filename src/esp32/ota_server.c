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

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "ota_server.h"
#include "../mcu_hw.h"

static const char *TAG = "ota_server";

/* Receive buffer size (bytes). Larger = faster upload but more heap. */
#define OTA_RECV_BUF 4096
#define FPGA_FLASH_BUF 4096
#define FPGA_FLASH_ADDR 0x100000  /* FPGA bitstream location in SPI flash */
#define FPGA_FLASH_SIZE 0x200000  /* 2 MB max for FPGA bitstream */

/* =========================================================================
 * GET / — status page
 * ========================================================================= */

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *app     = esp_app_get_description();

    char buf[512];
    int  n = snprintf(buf, sizeof(buf),
        "FPGA Companion OTA Server\r\n"
        "=========================\r\n"
        "Running partition : %s\r\n"
        "Firmware version  : %s\r\n"
        "Build date        : %s %s\r\n"
        "\r\n"
        "To upload new firmware:\r\n"
        "  curl -X POST http://<device-ip>:%d/update --data-binary @build/fpga_companion.bin\r\n",
        running ? running->label : "unknown",
        app->version,
        app->date, app->time,
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
    esp_err_t err;

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

    char *buf = malloc(FPGA_FLASH_BUF);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* Hold FPGA in reset during programming to ensure SPI pins are connected
     * and prevent flash access conflicts */
    ESP_LOGI(TAG, "Holding FPGA in reset for programming");
    gpio_set_direction(13, GPIO_MODE_OUTPUT);  // PIN_NUM_RECONFIG_N
    gpio_set_level(13, 0);  // Hold in reset
    vTaskDelay(pdMS_TO_TICKS(100));  // Let FPGA stabilize

    /* Acquire SPI flash semaphore for exclusive access */
    mcu_hw_spi_flash_begin();

    /* Erase flash region */
    ESP_LOGI(TAG, "Erasing flash region @ 0x%06x (%d bytes)",
             FPGA_FLASH_ADDR, FPGA_FLASH_SIZE);
    mcu_hw_erase_flash_region(FPGA_FLASH_ADDR, FPGA_FLASH_SIZE);
    ESP_LOGI(TAG, "Erase complete");

    /* Receive and write bitstream */
    ESP_LOGI(TAG, "Writing FPGA bitstream");
    int remaining = req->content_len;
    int written   = 0;
    uint32_t addr = FPGA_FLASH_ADDR;

    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf,
                                  remaining < FPGA_FLASH_BUF ? remaining : FPGA_FLASH_BUF);
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(buf);
            mcu_hw_spi_flash_end();
            gpio_set_level(13, 1);
            gpio_set_direction(13, GPIO_MODE_INPUT);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        mcu_hw_write_flash(addr, (uint8_t*)buf, recv);

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

    free(buf);
    mcu_hw_spi_flash_end();

    ESP_LOGI(TAG, "FPGA bitstream written successfully (%d bytes)", written);
    ESP_LOGI(TAG, "Releasing FPGA from reset - reconfiguration will start");

    /* Release FPGA from reset to trigger reconfiguration */
    gpio_set_level(13, 1);
    gpio_set_direction(13, GPIO_MODE_INPUT);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "FPGA update successful! FPGA reconfiguring from new bitstream...\r\n");

    ESP_LOGI(TAG, "FPGA OTA update complete");
    return ESP_OK;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void ota_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = CONFIG_OTA_PORT;
    cfg.max_uri_handlers = 3;  /* status, ESP32 update, FPGA update */
    cfg.stack_size       = 8192;

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

    ESP_LOGI(TAG, "OTA server ready on port %d", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  Status      : curl http://<device-ip>:%d/", CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  ESP32 upload: curl -X POST http://<device-ip>:%d/update --data-binary @build/fpga_companion.bin",
             CONFIG_OTA_PORT);
    ESP_LOGI(TAG, "  FPGA upload : curl -X POST http://<device-ip>:%d/fpga-update --data-binary @your_bitstream.bin",
             CONFIG_OTA_PORT);
}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */
