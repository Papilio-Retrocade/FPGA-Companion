/*
  bt_hid.h - BLE HID host for FPGA Companion (ESP32-S3)
*/

#ifndef BT_HID_H
#define BT_HID_H

#include <stdbool.h>

/**
 * @brief Initialise the BLE HID host stack and start background scan task.
 *
 * Must be called after WiFi / NVS initialisation (so the BT bonding store
 * in NVS is available) and before entering the main loop.
 * Internally calls esp_hid_gap_init(), registers the GATTC handler, calls
 * esp_hidh_init(), and spawns a FreeRTOS scan task that connects BLE HID
 * devices (keyboards, mice, gamepads, PS5 DualSense, Xbox Series controllers)
 * into the shared hid_parse() pipeline used by the USB HID path.
 */
void bt_hid_init(void);

/**
 * @brief Pause or resume BLE scanning.
 *
 * Call with pause=true before starting a long JTAG/WiFi transfer to prevent
 * BLE scanning from starving the WiFi radio and resetting TCP connections.
 * Call with pause=false when the transfer is complete.
 */
void bt_hid_set_scan_paused(bool pause);

#endif /* BT_HID_H */
