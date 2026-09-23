/*
 * usb_host_ctrl.c - BOOT-button USB Host mode toggle
 *
 * See usb_host_ctrl.h for the design. Independent of WiFi/wifi_log.c -- this
 * has to work even when WiFi logging is disabled at build time.
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "led_strip.h"

#include "usb_host_ctrl.h"

static const char *TAG = "usb_host_ctrl";

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define DEBOUNCE_MS      50
#define POLL_MS          20

/* SuperMini ESP32-S3 has the WS2812 on GPIO48 -- same pin wifi_log.c used to
 * drive before Phase 6; this module is now the sole owner of that LED. */
#ifndef CONFIG_WIFI_LOG_LED_GPIO
#define CONFIG_WIFI_LOG_LED_GPIO 48
#endif

static led_strip_handle_t s_led_strip    = NULL;
static bool               s_host_enabled = false;

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

static bool load_host_enabled(void)
{
    if (ensure_nvs_ready() != ESP_OK) return false;

    nvs_handle_t nvs;
    if (nvs_open("usb_ctrl", NVS_READONLY, &nvs) != ESP_OK) return false;

    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(nvs, "host_en", &v);
    nvs_close(nvs);

    return err == ESP_OK && v != 0;
}

static void save_host_enabled(bool enabled)
{
    if (ensure_nvs_ready() != ESP_OK) return;

    nvs_handle_t nvs;
    if (nvs_open("usb_ctrl", NVS_READWRITE, &nvs) != ESP_OK) return;

    if (nvs_set_u8(nvs, "host_en", enabled ? 1 : 0) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_WIFI_LOG_LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip) != ESP_OK) {
        s_led_strip = NULL;
    }
}

void usb_host_ctrl_update_led(void)
{
    if (!s_led_strip) return;
    if (s_host_enabled) {
        led_strip_set_pixel(s_led_strip, 0, 0, 16, 16);  /* turquoise */
    } else {
        led_strip_set_pixel(s_led_strip, 0, 0, 16, 0);   /* green */
    }
    led_strip_refresh(s_led_strip);
}

static void boot_button_task(void *arg)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_cfg);

    bool was_pressed = false;
    for (;;) {
        bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                /* Wait for release so one press produces exactly one toggle. */
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                }
                bool new_state = !s_host_enabled;
                ESP_LOGW(TAG, "BOOT button pressed - USB Host mode -> %s, rebooting to apply",
                         new_state ? "ENABLED" : "DISABLED");
                save_host_enabled(new_state);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void usb_host_ctrl_init(void)
{
    s_host_enabled = load_host_enabled();
    led_init();
    xTaskCreate(boot_button_task, "usb_host_btn", 2048, NULL, 5, NULL);
}

bool usb_host_ctrl_is_enabled(void)
{
    return s_host_enabled;
}
