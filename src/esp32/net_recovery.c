/*
 * net_recovery.c - Minimal network recovery/ROM-load HTTP server
 *
 * See net_recovery.h for what this replaces and why. No JTAG/SPI-flash
 * access happens here at all, so there's no mutex/busy-check needed (unlike
 * the old ota_server.c, which shared the FPGA SPI bridge with JTAG
 * programming).
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_NET_RECOVERY_ENABLE)

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_partition.h"

#include "net_recovery.h"
#include "../sdc.h"
#include "../osd.h"

static const char *TAG = "net_recovery";

/* =========================================================================
 * CORS / Private Network Access support (same pattern the old ota_server.c
 * used) -- lets a browser-hosted page (e.g. papilioworks.com/flash) call
 * these endpoints directly via fetch() from a different origin.
 * ========================================================================= */

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", CONFIG_NET_RECOVERY_CORS_ALLOW_ORIGIN);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Content-Length");
}

static esp_err_t net_recovery_send_err_impl(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    add_cors_headers(req);
    return httpd_resp_send_err(req, error, msg);
}

static esp_err_t net_recovery_send_str_impl(httpd_req_t *req, const char *msg)
{
    add_cors_headers(req);
    return httpd_resp_sendstr(req, msg);
}

#define httpd_resp_send_err(req, error, msg) net_recovery_send_err_impl(req, error, msg)
#define httpd_resp_sendstr(req, msg)         net_recovery_send_str_impl(req, msg)

/* 503 Service Unavailable helper -- httpd_err_code_t has no 503 constant */
static esp_err_t send_service_unavailable(httpd_req_t *req, const char *msg)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, strlen(msg));
    return ESP_FAIL;
}

/* Generic OPTIONS preflight handler, shared by every POST endpoint below. */
static esp_err_t handle_options_preflight(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================================
 * GET / — status page
 * ========================================================================= */

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *app     = esp_app_get_description();

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "FPGA Companion - Network Recovery\r\n"
        "==================================\r\n"
        "Running partition : %s\r\n"
        "Firmware version  : %s\r\n"
        "Build date        : %s %s\r\n"
        "\r\n"
        "Firmware/FPGA updates are handled by papilio-esp-bootloader now.\r\n"
        "\r\n"
        "  Reboot into the loader:\r\n"
        "    curl -X POST http://<device-ip>:%d/goto-loader\r\n"
        "\r\n"
        "  Load a ROM/cart/disk image onto the SD card and hot-insert it (drive 0):\r\n"
        "    curl -X POST \"http://<device-ip>:%d/rom-load?name=game.a26\" --data-binary @game.a26\r\n",
        running ? running->label : "unknown",
        app->version,
        app->date, app->time,
        CONFIG_NET_RECOVERY_PORT,
        CONFIG_NET_RECOVERY_PORT);

    add_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* =========================================================================
 * POST /goto-loader
 *
 * Sets the boot partition to the "factory" slot (papilio-esp-bootloader)
 * and reboots into it. Lets host tooling recover/re-provision the device
 * over the network without needing physical BOOT-button/USB access.
 * ========================================================================= */

static esp_err_t handle_goto_loader(httpd_req_t *req)
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        return net_recovery_send_err_impl(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "No factory (loader) partition found on this board's partition table");
    }

    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(factory) failed: %s", esp_err_to_name(err));
        return net_recovery_send_err_impl(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to set boot partition to factory");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Rebooting into papilio-esp-bootloader...\r\n");

    ESP_LOGW(TAG, "POST /goto-loader - rebooting into factory partition");
    vTaskDelay(pdMS_TO_TICKS(300));  /* let the response flush before reboot */
    esp_restart();
    return ESP_OK;  /* unreachable */
}

/* =========================================================================
 * POST /rom-load?name=<filename>[&insert=0]
 *
 * Uploads a ROM/cart/disk image to /roms/<filename> on the SD card, then
 * hot-inserts it into drive 0 via sdc_image_open() -- exactly what the OSD
 * file browser does when a user picks a file. Works on every core (A2600,
 * NES, SNES, C64, ...) that reads images from the SD card. Pass ?insert=0
 * to upload without mounting.
 *
 * Usage:
 *   curl -X POST "http://<device-ip>:3232/rom-load?name=frogger.a26" --data-binary @frogger.a26
 * ========================================================================= */

#define ROM_LOAD_MAX_SIZE (1024 * 1024) /* 1 MB cap -- covers every cart/disk-style core */
#define ROM_LOAD_DRIVE    0             /* cartridge/ROM slot for A2600/NES/SNES */
#define ROM_LOAD_DIR      CARD_MOUNTPOINT "/roms"
#define ROM_LOAD_RECV_BUF 4096

/* Reject path separators/traversal -- caller must supply a bare filename */
static bool rom_load_name_is_safe(const char *name)
{
    if (!name || !*name) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    if (!strchr(name, '.')) return false;  /* require an extension */
    if (strlen(name) >= FF_LFN_BUF) return false;
    return true;
}

static esp_err_t handle_rom_load(httpd_req_t *req)
{
    char query[192]       = {0};
    char name[FF_LFN_BUF] = {0};
    char insert_str[8]    = {0};
    bool do_insert        = true;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing required query parameter: ?name=game.a26");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "insert", insert_str, sizeof(insert_str)) == ESP_OK)
        do_insert = (strcmp(insert_str, "0") != 0);

    if (!rom_load_name_is_safe(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid name: use a bare filename with an extension, no '/', '\\' or '..'");
        return ESP_FAIL;
    }

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (req->content_len > ROM_LOAD_MAX_SIZE) {
        ESP_LOGE(TAG, "ROM too large: %d bytes (max %d)", req->content_len, ROM_LOAD_MAX_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data too large (max 1 MB)");
        return ESP_FAIL;
    }

    char path[sizeof(ROM_LOAD_DIR) + FF_LFN_BUF + 2];
    snprintf(path, sizeof(path), ROM_LOAD_DIR "/%s", name);

    ESP_LOGI(TAG, "ROM load: %d bytes -> %s", req->content_len, path);

    sdc_lock();
    f_mkdir(ROM_LOAD_DIR);  /* ignore error: FR_EXIST if it's already there */

    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        sdc_unlock();
        ESP_LOGE(TAG, "f_open failed for %s (no SD card?)", path);
        return send_service_unavailable(req, "SD card write failed (no card inserted?)\r\n");
    }

    char *buf = malloc(ROM_LOAD_RECV_BUF);
    if (!buf) {
        f_close(&fil);
        sdc_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int  remaining = req->content_len;
    int  written   = 0;
    bool recv_err  = false;

    while (remaining > 0) {
        int to_recv = (remaining < ROM_LOAD_RECV_BUF) ? remaining : ROM_LOAD_RECV_BUF;
        int recv    = httpd_req_recv(req, buf, to_recv);
        if (recv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d) after %d bytes", recv, written);
            recv_err = true;
            break;
        }

        UINT bw = 0;
        if (f_write(&fil, buf, recv, &bw) != FR_OK || bw != (UINT)recv) {
            ESP_LOGE(TAG, "SD write error after %d bytes", written);
            recv_err = true;
            break;
        }

        written   += recv;
        remaining -= recv;
    }

    free(buf);
    f_close(&fil);

    if (recv_err) {
        f_unlink(path);  /* don't leave a half-written ROM the OSD could later pick up */
        sdc_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed, partial file removed");
        return ESP_FAIL;
    }

    sdc_unlock();

    ESP_LOGI(TAG, "ROM write complete: %d bytes -> %s", written, path);

    bool osd_open = osd_is_visible();
    bool inserted = false;
    if (do_insert && !osd_open) {
        /* sdc_set_default() points drive 0's cwd at /roms; sdc_image_open()
         * then opens the file and reports it to the core -- same call the
         * OSD file browser makes when a user picks a file. */
        sdc_set_default(ROM_LOAD_DRIVE, path);
        if (sdc_image_open(ROM_LOAD_DRIVE, name) == 0) {
            inserted = true;
        } else {
            ESP_LOGE(TAG, "sdc_image_open failed for %s", path);
        }
    } else if (do_insert && osd_open) {
        ESP_LOGW(TAG, "OSD is open -- file saved but skipping hot-insert to avoid a race");
    }

    const char *status_msg =
             !do_insert ? "Upload only (insert=0), not mounted." :
             inserted   ? "Cartridge inserted." :
             osd_open   ? "OSD is open; file saved but not mounted." :
                          "Mount failed (core not ready?); file saved.";
    char resp[sizeof(path) + 128];
    snprintf(resp, sizeof(resp),
             "ROM upload successful! %d bytes -> %s\r\n%s\r\n",
             written, path, status_msg);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, resp);

    return ESP_OK;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void net_recovery_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = CONFIG_NET_RECOVERY_PORT;
    /* status, goto-loader, rom-load, plus one OPTIONS preflight for each POST endpoint. */
    cfg.max_uri_handlers  = 5;
    cfg.stack_size        = 8192;
    cfg.recv_wait_timeout = 15;  /* 15 s per recv -- long stalls indicate a dead TCP connection */
    cfg.send_wait_timeout = 15;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", CONFIG_NET_RECOVERY_PORT);
        return;
    }

    static const httpd_uri_t status_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_status,
    };
    httpd_register_uri_handler(server, &status_uri);

    static const httpd_uri_t goto_loader_uri = {
        .uri     = "/goto-loader",
        .method  = HTTP_POST,
        .handler = handle_goto_loader,
    };
    httpd_register_uri_handler(server, &goto_loader_uri);

    static const httpd_uri_t rom_load_uri = {
        .uri     = "/rom-load",
        .method  = HTTP_POST,
        .handler = handle_rom_load,
    };
    httpd_register_uri_handler(server, &rom_load_uri);

    /* OPTIONS preflight for every POST endpoint (Chrome sends this before
     * each cross-origin POST when Private Network Access is in play). */
    static const char *cors_paths[] = { "/goto-loader", "/rom-load" };
    static httpd_uri_t options_uris[2];
    for (size_t i = 0; i < sizeof(cors_paths) / sizeof(cors_paths[0]); i++) {
        options_uris[i].uri     = cors_paths[i];
        options_uris[i].method  = HTTP_OPTIONS;
        options_uris[i].handler = handle_options_preflight;
        httpd_register_uri_handler(server, &options_uris[i]);
    }

    ESP_LOGI(TAG, "Network recovery server ready on port %d", CONFIG_NET_RECOVERY_PORT);
    ESP_LOGI(TAG, "  CORS origin  : %s", CONFIG_NET_RECOVERY_CORS_ALLOW_ORIGIN);
    ESP_LOGI(TAG, "  Status       : curl http://<device-ip>:%d/", CONFIG_NET_RECOVERY_PORT);
    ESP_LOGI(TAG, "  Goto loader  : curl -X POST http://<device-ip>:%d/goto-loader", CONFIG_NET_RECOVERY_PORT);
    ESP_LOGI(TAG, "  ROM load (SD): curl -X POST \"http://<device-ip>:%d/rom-load?name=game.a26\" --data-binary @game.a26",
             CONFIG_NET_RECOVERY_PORT);
}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_NET_RECOVERY_ENABLE */
