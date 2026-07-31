/*
 * wifi_provision.c - WiFi credential provisioning over USB serial
 *
 * See wifi_provision.h for the line protocol. Runs as a small background
 * task that blocks on stdin (the console UART/USB), so it costs nothing
 * once idle and never interferes with the wifi_log UDP path.
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE)

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_provision.h"

static const char *TAG = "wifi_provision";

#define PROVISION_LINE_MAX 96

/* wifi_provision_start() runs before wifi_log_init()'s nvs_flash_init(), so
 * ensure NVS is ready here too (nvs_flash_init() is safe to call again). */
static esp_err_t ensure_nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t save_credential(const char *key, const char *value)
{
    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) return err;

    nvs_handle_t nvs;
    err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);

    nvs_close(nvs);
    return err;
}

/* Blocks reading one newline-terminated line from stdin. Returns the number
 * of characters read (excluding the newline), or 0 for an empty line.
 *
 * getchar() is expected to block until a byte arrives, but if stdin isn't
 * actually connected to a live console (e.g. no serial monitor attached)
 * some VFS backends return EOF immediately instead of blocking. Without a
 * delay here that turns into a tight loop that starves the idle task and
 * trips the watchdog, so back off briefly on EOF rather than retrying instantly.
 */
static size_t read_line(char *buf, size_t buf_size)
{
    size_t len = 0;
    int c;

    for (;;) {
        c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') break;
        if (len < buf_size - 1) buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return len;
}

static void provision_task(void *arg)
{
    char line[PROVISION_LINE_MAX];
    bool got_ssid = false, got_pass = false;

    ESP_LOGI(TAG, "Listening on serial for WIFI_SSID=/WIFI_PASS= provisioning commands");

    for (;;) {
        size_t len = read_line(line, sizeof(line));
        if (len == 0) continue;

        if (strncmp(line, "WIFI_SSID=", 10) == 0) {
            if (save_credential("ssid", line + 10) == ESP_OK) {
                got_ssid = true;
                printf("WIFI_CFG_OK ssid\r\n");
            } else {
                printf("WIFI_CFG_ERR ssid\r\n");
            }
        } else if (strncmp(line, "WIFI_PASS=", 10) == 0) {
            if (save_credential("pass", line + 10) == ESP_OK) {
                got_pass = true;
                printf("WIFI_CFG_OK pass\r\n");
            } else {
                printf("WIFI_CFG_ERR pass\r\n");
            }
        }

        if (got_ssid && got_pass) {
            printf("WIFI_CFG_OK reboot\r\n");
            fflush(stdout);
            ESP_LOGI(TAG, "WiFi credentials saved to NVS, rebooting to connect");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
}

void wifi_provision_start(void)
{
    xTaskCreate(provision_task, "wifi_provision", 4096, NULL, 5, NULL);
}

bool wifi_provision_is_configured(void)
{
    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) return false;

    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) return false;

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    err = nvs_get_str(nvs, "ssid", ssid, &len);
    nvs_close(nvs);

    return err == ESP_OK && ssid[0] != '\0';
}

#endif /* CONFIG_WIFI_LOG_ENABLE */
