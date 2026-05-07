/*
 * wifi_log.c - UDP wireless logging for FPGA Companion
 *
 * Connects to WiFi and forwards all printf/debugf output to UDP broadcast
 * packets on port 7777. This lets you see logs wirelessly when USB is
 * occupied by USB Host mode (HID keyboards/gamepads).
 *
 * Receiving logs on your PC:
 *   Windows (nmap/ncat): ncat -u -l 7777
 *   Linux/macOS:         nc -u -l -p 7777
 *   Python snippet:
 *     import socket
 *     s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
 *     s.bind(('', 7777))
 *     while True:
 *         data, addr = s.recvfrom(4096)
 *         print(data.decode(errors='replace'), end='')
 *
 * The vprintf hook is installed immediately at wifi_log_early_init() so that
 * ALL log output (including pre-WiFi boot messages) is captured in a ring
 * buffer and flushed to UDP once the connection is established.
 */

#include "sdkconfig.h"
#ifdef CONFIG_WIFI_LOG_ENABLE

#include <string.h>
#include <stdio.h>

#include <reent.h>  /* struct _reent for __wrap__write_r */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "wifi_log.h"

/* ========================================================================= */

#define WIFI_LOG_UDP_PORT            7777
#define WIFI_LOG_MAX_RETRIES         5
#define WIFI_LOG_CONNECT_TIMEOUT_MS  15000
#define WIFI_LOG_LINE_BUF            512
#define WIFI_LOG_RING_BUF_BYTES      8192   /* pre-WiFi backlog */

static const char *TAG = "wifi_log";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_num        = 0;
static int                s_udp_sock         = -1;
static struct sockaddr_in s_dest_addr;
static RingbufHandle_t    s_ringbuf      = NULL;
static TaskHandle_t       s_sender_task  = NULL;

/* ========================================================================= */
/* Async UDP sender task                                                       */
/*                                                                             */
/* Drains the ring buffer and sends UDP packets in its own task context so    */
/* __wrap__write_r never blocks on network I/O. The task wakes on notification*/
/* from the hook, or polls every 50 ms to catch any missed notifications.     */
/* ========================================================================= */

#define WIFI_LOG_UDP_CHUNK  1400  /* stay under Ethernet MTU */

static void udp_sender_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        if (s_udp_sock < 0 || !s_ringbuf) continue;

        size_t   item_size;
        uint8_t *item;
        while ((item = (uint8_t *)xRingbufferReceiveUpTo(
                        s_ringbuf, &item_size, 0, WIFI_LOG_UDP_CHUNK)) != NULL) {
            sendto(s_udp_sock, item, item_size, 0,
                   (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
            vRingbufferReturnItem(s_ringbuf, item);
        }
    }
}

/* ========================================================================= */
/* __wrap__write_r - intercepts ALL printf AND ESP_LOG output                 */
/*                                                                             */
/* Both printf() and ESP_LOGx() ultimately call _write_r() to flush bytes    */
/* to stdout (fd==1). The hook queues bytes non-blocking into the ring buffer */
/* and wakes the sender task; it never touches the network itself.            */
/*                                                                             */
/* CMakeLists.txt adds: -Wl,--wrap=_write_r                                  */
/* ========================================================================= */

extern ssize_t __real__write_r(struct _reent *r, int fd, const void *buf, size_t nbytes);

ssize_t __wrap__write_r(struct _reent *r, int fd, const void *buf, size_t nbytes) {
    ssize_t ret = __real__write_r(r, fd, buf, nbytes);

    if ((fd == 1 || fd == 2) && nbytes > 0 && s_ringbuf
            && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        if (xRingbufferSend(s_ringbuf, buf, nbytes, 0) == pdTRUE && s_sender_task) {
            xTaskNotifyGive(s_sender_task);
        }
    }
    return ret;
}

/* ========================================================================= */
/* WiFi event handler                                                          */
/* ========================================================================= */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_LOG_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)...", s_retry_num, WIFI_LOG_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi connection failed after %d retries", WIFI_LOG_MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ========================================================================= */
/* Public API                                                                  */
/* ========================================================================= */

void wifi_log_early_init(void) {
    /* Create ring buffer and sender task so all output is captured from here
     * onwards. The sender task blocks until the UDP socket opens, then
     * automatically drains the pre-WiFi backlog. */
    s_ringbuf = xRingbufferCreate(WIFI_LOG_RING_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    xTaskCreate(udp_sender_task, "udp_log", 4096, NULL, 5, &s_sender_task);
}

void wifi_log_init(void) {
    /* NVS is required by the WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_WIFI_LOG_SSID,
            .password = CONFIG_WIFI_LOG_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    /* Wait for connection (or failure) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_LOG_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi log: could not connect - logs will stay on UART only");
        return;
    }

    /* Open UDP broadcast socket */
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    int broadcast = 1;
    setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family      = AF_INET;
    s_dest_addr.sin_port        = htons(WIFI_LOG_UDP_PORT);
    s_dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    /* Sender task drains the pre-WiFi backlog automatically now that
     * s_udp_sock >= 0; give it an immediate nudge. */
    if (s_sender_task) xTaskNotifyGive(s_sender_task);

    ESP_LOGI(TAG, "WiFi UDP logging active - listen with: ncat -u -l %d  (or nc -u -l -p %d)",
             WIFI_LOG_UDP_PORT, WIFI_LOG_UDP_PORT);
}

#endif /* CONFIG_WIFI_LOG_ENABLE */
