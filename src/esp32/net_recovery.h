#ifndef NET_RECOVERY_H
#define NET_RECOVERY_H

/*
 * net_recovery.h - Minimal network recovery/ROM-load HTTP server
 *
 * Phase 6 of the papilio-esp-bootloader plan moved all OTA/JTAG/serial-flash
 * firmware-update code out of FPGA-Companion and into the papilio-esp-
 * bootloader factory app, which now owns re-flashing this app's own OTA
 * slot. This file keeps just two small, low-risk network endpoints that
 * don't touch JTAG or the FPGA SPI flash at all:
 *
 *   GET  /             - status text
 *   POST /goto-loader  - reboot into the papilio-esp-bootloader factory
 *                        partition, so a remote tool can force the device
 *                        back to the loader over the network (e.g. to push
 *                        a new build) without needing physical/USB access.
 *   POST /rom-load     - upload a ROM/cart/disk image to the SD card and
 *                        hot-insert it (same as picking a file in the OSD
 *                        file browser).
 *
 * Requires WIFI_LOG_ENABLE (WiFi connection infrastructure).
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_NET_RECOVERY_ENABLE)

/**
 * Start the recovery HTTP server. Call this after WiFi is connected.
 * Listens on CONFIG_NET_RECOVERY_PORT (default 3232).
 */
void net_recovery_start(void);

#else
static inline void net_recovery_start(void) {}
#endif

#endif /* NET_RECOVERY_H */
