/*
  bt_hid.c - BLE HID host for FPGA Companion (ESP32-S3)

  Scans for BLE HID devices (keyboards, mice, gamepads, PS5 DualSense,
  Xbox Series X/S) and feeds their input reports into the same hid_parse()
  pipeline used by the USB HID path in mcu_hw.c.

  Pairing/bonding is handled transparently by the ESP-IDF esp_hidh stack
  using NVS storage, so paired devices reconnect automatically on reboot.

  Xbox Series X/S: put controller into BLE pairing mode by holding the
  small Pair button on the top of the controller until the Xbox logo flashes.
  PS5 DualSense: hold CREATE + PS button until light bar flashes.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_gattc_api.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "esp_hid_gap.h"

#include "../hidparser.h"
#include "../hid.h"
#include "../debug.h"

#define TAG "BT_HID"

/* Number of concurrent BLE HID devices supported */
#define MAX_BT_HID_DEVICES   4

/* Initial scan duration on boot (seconds) */
#define BT_SCAN_DURATION_S   5

/* Re-scan interval when slots are available (milliseconds) */
#define BT_RESCAN_MS         10000

#include "esp_gap_ble_api.h"

/* Set to true to pause BLE scanning (e.g. during WiFi-intensive JTAG transfers) */
static volatile bool s_scan_paused = false;

void bt_hid_set_scan_paused(bool pause)
{
    s_scan_paused = pause;
    if (pause) {
        /* If a scan is currently in flight, cancel it immediately so we
         * don't have to wait up to BT_SCAN_DURATION_S seconds for it to
         * finish.  An active BLE scan starves WiFi RX on the shared radio
         * and causes TCP stalls during the OTA flash write. */
        esp_ble_gap_stop_scanning();
    }
}

/* -------------------------------------------------------------------------
 * Device table — mirrors the USB hid_device[] table in mcu_hw.c
 * ------------------------------------------------------------------------- */
static struct {
    esp_hidh_dev_t *dev;
    hid_state_t     state;
    hid_report_t    rep;
} bt_dev[MAX_BT_HID_DEVICES];

/* -------------------------------------------------------------------------
 * HID host event callback
 * Runs inside the esp_hidh event task (separate FreeRTOS task).
 * ------------------------------------------------------------------------- */
static void hidh_cb(void *handler_args, esp_event_base_t base,
                    int32_t id, void *event_data)
{
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {

    /* ------------------------------------------------------------------ */
    case ESP_HIDH_OPEN_EVENT: {
        esp_hidh_dev_t *dev   = param->open.dev;
        const char     *name  = esp_hidh_dev_name_get(dev);
        ESP_LOGI(TAG, "OPEN: %s", name ? name : "(unknown)");

        /* Find a free table slot */
        int idx;
        for (idx = 0; idx < MAX_BT_HID_DEVICES; idx++)
            if (bt_dev[idx].dev == NULL) break;

        if (idx == MAX_BT_HID_DEVICES) {
            ESP_LOGW(TAG, "No free BT HID slots — ignoring device");
            break;
        }

        /* Obtain and parse the HID report descriptor */
        size_t                   num_maps = 0;
        esp_hid_raw_report_map_t *maps    = NULL;
        esp_err_t rc = esp_hidh_dev_report_maps_get(dev, &num_maps, &maps);

        if (rc != ESP_OK || num_maps == 0 || maps == NULL ||
            maps[0].data == NULL || maps[0].len == 0) {
            ESP_LOGW(TAG, "BT HID[%d]: no report map (rc=%d)", idx, rc);
            break;
        }

        if (!parse_report_descriptor(maps[0].data, maps[0].len,
                                     &bt_dev[idx].rep, NULL)) {
            ESP_LOGW(TAG, "BT HID[%d]: report descriptor parse failed", idx);
            break;
        }

        hid_report_t *r = &bt_dev[idx].rep;
        ESP_LOGI(TAG, "BT HID[%d] type=%d id_present=%d id=%d size=%d",
                 idx, r->type, r->report_id_present, r->report_id, r->report_size);
        /* Log axis layout */
        for (int a = 0; a < MAX_AXES; a++)
            if (r->joystick_mouse.axis[a].size)
                ESP_LOGI(TAG, "  axis[%d]: offset=%d size=%d min=%d max=%d",
                         a,
                         r->joystick_mouse.axis[a].offset,
                         r->joystick_mouse.axis[a].size,
                         r->joystick_mouse.axis[a].logical.min,
                         r->joystick_mouse.axis[a].logical.max);
        /* Log button layout */
        for (int b = 0; b < 16; b++)
            if (r->joystick_mouse.button[b].bitmask)
                ESP_LOGI(TAG, "  btn[%d]: byte=%d mask=0x%02x",
                         b,
                         r->joystick_mouse.button[b].byte_offset,
                         r->joystick_mouse.button[b].bitmask);
        /* Log HAT layout */
        if (r->joystick_mouse.hat.size)
            ESP_LOGI(TAG, "  hat: offset=%d size=%d min=%d max=%d",
                     r->joystick_mouse.hat.offset,
                     r->joystick_mouse.hat.size,
                     r->joystick_mouse.hat.logical.min,
                     r->joystick_mouse.hat.logical.max);

        /* Commit slot */
        bt_dev[idx].dev = dev;
        if (bt_dev[idx].rep.type == REPORT_TYPE_JOYSTICK)
            bt_dev[idx].state.joystick.js_index = hid_allocate_joystick();
        break;
    }

    /* ------------------------------------------------------------------ */
    case ESP_HIDH_INPUT_EVENT: {
        esp_hidh_dev_t *dev = param->input.dev;
        for (int idx = 0; idx < MAX_BT_HID_DEVICES; idx++) {
            if (bt_dev[idx].dev != dev) continue;

            hid_report_t *rep = &bt_dev[idx].rep;

            /* Dump raw bytes for the first 10 reports to aid button mapping debug */
            static int dbg_count[MAX_BT_HID_DEVICES];
            if (dbg_count[idx] < 10) {
                dbg_count[idx]++;
                uint16_t dlen = param->input.length > 20 ? 20 : param->input.length;
                char hex[64]; hex[0] = '\0';
                for (int b = 0; b < dlen; b++) {
                    char tmp[4];
                    snprintf(tmp, sizeof(tmp), "%02x ", param->input.data[b]);
                    strncat(hex, tmp, sizeof(hex) - strlen(hex) - 1);
                }
                ESP_LOGI(TAG, "RAW[%d] rid=%d len=%d: %s",
                         idx, param->input.report_id, param->input.length, hex);
            }

            if (rep->report_id_present) {
                /*
                 * USB HID includes the report ID as the first byte of the
                 * payload. BLE HID (esp_hidh) strips the report ID — it is
                 * delivered in param->input.report_id and data[] starts with
                 * the actual report payload. Reconstruct the prefixed buffer
                 * so hid_parse() sees the same layout as the USB path.
                 */
                uint16_t full_len = param->input.length + 1;
                uint8_t  buf[full_len];
                buf[0] = (uint8_t)param->input.report_id;
                memcpy(buf + 1, param->input.data, param->input.length);
                hid_parse(rep, &bt_dev[idx].state, buf, full_len);
            } else {
                hid_parse(rep, &bt_dev[idx].state,
                          param->input.data, param->input.length);
            }
            break;
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case ESP_HIDH_CLOSE_EVENT: {
        esp_hidh_dev_t *dev  = param->close.dev;
        const char     *name = esp_hidh_dev_name_get(dev);
        ESP_LOGI(TAG, "CLOSE: %s reason=%d",
                 name ? name : "(unknown)", param->close.reason);

        for (int idx = 0; idx < MAX_BT_HID_DEVICES; idx++) {
            if (bt_dev[idx].dev == dev) {
                if (bt_dev[idx].rep.type == REPORT_TYPE_JOYSTICK)
                    hid_release_joystick(bt_dev[idx].state.joystick.js_index);
                bt_dev[idx].dev = NULL;
                memset(&bt_dev[idx].rep,   0, sizeof(hid_report_t));
                memset(&bt_dev[idx].state, 0, sizeof(hid_state_t));
                break;
            }
        }
        /* Required: release esp_hidh internal memory for this device */
        esp_hidh_dev_free(dev);
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Background scan task
 * Runs an initial scan after the stack stabilises, then rescans every
 * BT_RESCAN_MS milliseconds as long as there are free device slots.
 * ------------------------------------------------------------------------- */
static void bt_hid_scan_task(void *arg)
{
    /* Give the BT stack time to complete initialisation */
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;) {
        /* Count free slots */
        int free_slots = 0;
        for (int i = 0; i < MAX_BT_HID_DEVICES; i++)
            if (bt_dev[i].dev == NULL) free_slots++;

        if (free_slots > 0) {
            if (s_scan_paused) {
                vTaskDelay(pdMS_TO_TICKS(BT_RESCAN_MS));
                continue;
            }
            ESP_LOGI(TAG, "Scanning for BLE HID devices (%ds)...",
                     BT_SCAN_DURATION_S);

            size_t                num_results = 0;
            esp_hid_scan_result_t *results    = NULL;
            esp_hid_scan(BT_SCAN_DURATION_S, &num_results, &results);

            for (esp_hid_scan_result_t *r = results; r; r = r->next) {
                if (r->transport != ESP_HID_TRANSPORT_BLE) continue;

                /* Skip devices already in the table */
                bool already = false;
                for (int i = 0; i < MAX_BT_HID_DEVICES; i++) {
                    if (bt_dev[i].dev == NULL) continue;
                    const uint8_t *bda = esp_hidh_dev_bda_get(bt_dev[i].dev);
                    if (bda && memcmp(bda, r->bda, 6) == 0) {
                        already = true;
                        break;
                    }
                }
                if (already) continue;

                ESP_LOGI(TAG, "Connecting: %s",
                         r->name ? r->name : "(unknown)");
                esp_hidh_dev_open(r->bda, r->transport, r->ble.addr_type);
            }

            esp_hid_scan_results_free(results);
        }

        vTaskDelay(pdMS_TO_TICKS(BT_RESCAN_MS));
    }
}

/* -------------------------------------------------------------------------
 * Public init function — called once from mcu_hw_init()
 * ------------------------------------------------------------------------- */
void bt_hid_init(void)
{
    memset(bt_dev, 0, sizeof(bt_dev));

    /* Initialise BT controller + Bluedroid + GAP for BLE HID host mode */
    ESP_ERROR_CHECK(esp_hid_gap_init(HID_HOST_MODE));

    /* Register the GATTC event handler required by esp_hidh for BLE */
    ESP_ERROR_CHECK(
        esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    /* Initialise the HID host — this starts the internal event task */
    const esp_hidh_config_t config = {
        .callback         = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    /* Scan task: finds and connects BLE HID peripherals */
    xTaskCreate(bt_hid_scan_task, "bt_hid_scan", 4096, NULL, 2, NULL);
}
