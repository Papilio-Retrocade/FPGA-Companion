#ifndef WIFI_PROVISION_H
#define WIFI_PROVISION_H

/*
 * wifi_provision.h - WiFi credential provisioning over USB serial
 *
 * Lets a host tool (e.g. the hosted web flasher's esptool-js session) send
 * WiFi credentials to the device over the same serial port used to flash
 * firmware, without a rebuild or a separate NVS-image flashing step.
 *
 * Protocol (line-based, newline terminated, sent over the console UART/USB):
 *   WIFI_SSID=<ssid>\n
 *   WIFI_PASS=<password>\n
 *   USB_HOST_HOLD\n
 *   USB_HOST_RESUME\n
 *
 * Credentials are stored in NVS namespace "wifi_cfg" (keys "ssid"/"pass") -
 * the same location used by the manual esptool + nvs_partition_gen workflow
 * documented in WIFI_NVS_PROVISIONING.md. Once both values are received the
 * device acknowledges over serial and reboots to connect with the new
 * credentials.
 *
 * USB_HOST_HOLD/USB_HOST_RESUME let a developer keep the USB-Serial/JTAG
 * console alive for debugging on any board with CONFIG_USB_HOST_ENABLE,
 * even one that already has WiFi provisioned (which would otherwise switch
 * to USB Host mode almost immediately). The choice is persisted in NVS
 * ('usb_hold' key) so it survives reboots until explicitly resumed.
 */

#include "sdkconfig.h"
#include <stdbool.h>

#if defined(CONFIG_WIFI_LOG_ENABLE)

/**
 * Start the background task that listens for WIFI_SSID=/WIFI_PASS= lines on
 * the console UART/USB and persists them to NVS. Safe to call once at boot,
 * before or after WiFi connects — it runs independently of the WiFi state.
 */
void wifi_provision_start(void);

/**
 * True once WiFi credentials have been saved to NVS by a previous
 * provisioning cycle (i.e. the "ssid" key exists in the "wifi_cfg"
 * namespace). Callers use this to decide whether it's safe to bring up USB
 * Host mode: on this hardware, USB Host reuses the same GPIO19/20 pins as
 * the USB-Serial/JTAG console, so bringing it up before WiFi is configured
 * would cut off the only channel available to provision the device.
 */
bool wifi_provision_is_configured(void);

/**
 * True if USB Host bring-up should be held off right now: either WiFi isn't
 * configured yet, or USB_HOST_HOLD was requested over serial (persisted in
 * NVS across reboots until USB_HOST_RESUME).
 */
bool wifi_provision_should_hold_usb_host(void);

/**
 * Registers the function that brings up USB Host mode. If USB Host was held
 * off at boot, sending "USB_HOST_RESUME" over serial invokes this
 * immediately instead of requiring a reboot.
 */
void wifi_provision_set_usb_resume_callback(void (*cb)(void));

/**
 * Tells wifi_provision that USB Host has already been started (normal boot
 * path), so a later USB_HOST_RESUME command becomes a no-op instead of
 * calling the resume callback a second time.
 */
void wifi_provision_notify_usb_started(void);

#else

static inline void wifi_provision_start(void) {}
static inline bool wifi_provision_is_configured(void) { return true; }
static inline bool wifi_provision_should_hold_usb_host(void) { return false; }
static inline void wifi_provision_set_usb_resume_callback(void (*cb)(void)) { (void)cb; }
static inline void wifi_provision_notify_usb_started(void) {}

#endif /* CONFIG_WIFI_LOG_ENABLE */

#endif /* WIFI_PROVISION_H */
