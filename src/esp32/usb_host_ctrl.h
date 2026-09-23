#ifndef USB_HOST_CTRL_H
#define USB_HOST_CTRL_H

/*
 * usb_host_ctrl.h - BOOT-button USB Host mode toggle
 *
 * The ESP32-S3's D+/D- pins are shared between the native USB Serial/JTAG
 * console and USB Host mode (game controllers) -- switching to Host mode is
 * a one-way trip until reboot. Rather than a serial command (the old
 * wifi_provision.c USB_HOST_HOLD/USB_HOST_RESUME protocol, removed along
 * with the rest of the OTA/WiFi-provisioning stack in Phase 6), this reads
 * a flag persisted in FPGA-Companion's own NVS partition at boot to decide
 * which mode to start in, and watches the BOOT button (GPIO0, unaffected by
 * whichever USB mode is active) at runtime: a press toggles the flag and
 * reboots to apply it.
 *
 * Status LED (WS2812, CONFIG_WIFI_LOG_LED_GPIO):
 *   green      = USB Host disabled (serial/JTAG console available)
 *   turquoise  = USB Host enabled
 */

#include <stdbool.h>

/* Reads the persisted flag and starts the BOOT-button watcher task. Call
 * once from mcu_hw_init(), before deciding whether to bring up USB Host. */
void usb_host_ctrl_init(void);

/* True if USB Host mode should be enabled this boot. */
bool usb_host_ctrl_is_enabled(void);

/* Sets the status LED to reflect the current mode. Call once steady state
 * is reached (main loop entry) so it doesn't fight with any other boot-time
 * indicator sharing the same LED. */
void usb_host_ctrl_update_led(void);

#endif /* USB_HOST_CTRL_H */
