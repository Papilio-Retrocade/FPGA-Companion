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
 *
 * Credentials are stored in NVS namespace "wifi_cfg" (keys "ssid"/"pass") -
 * the same location used by the manual esptool + nvs_partition_gen workflow
 * documented in WIFI_NVS_PROVISIONING.md. Once both values are received the
 * device acknowledges over serial and reboots to connect with the new
 * credentials.
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

#else

static inline void wifi_provision_start(void) {}
static inline bool wifi_provision_is_configured(void) { return true; }

#endif /* CONFIG_WIFI_LOG_ENABLE */

#endif /* WIFI_PROVISION_H */
