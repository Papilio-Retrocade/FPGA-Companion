#ifndef OTA_SERVER_H
#define OTA_SERVER_H

/*
 * ota_server.h - HTTP OTA firmware update server for FPGA Companion
 *
 * Starts a lightweight HTTP server that accepts firmware uploads via POST,
 * writes them to the inactive OTA partition, and reboots into the new image.
 *
 * Requires WIFI_LOG_ENABLE (WiFi connection infrastructure) and a partition
 * table with two OTA app slots (see partitions_ota.csv).
 *
 * Usage after flashing:
 *   curl -X POST http://<device-ip>:3232/update --data-binary @build/fpga_companion.bin
 *
 * Status / current version:
 *   curl http://<device-ip>:3232/
 *
 * The device IP is printed to the log when WiFi connects.
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_LOG_ENABLE) && defined(CONFIG_OTA_ENABLE)

/**
 * Start the OTA HTTP server. Call this after WiFi is connected.
 * The server listens on CONFIG_OTA_PORT (default 3232).
 */
void ota_server_start(void);

#else

static inline void ota_server_start(void) {}

#endif /* CONFIG_WIFI_LOG_ENABLE && CONFIG_OTA_ENABLE */

#endif /* OTA_SERVER_H */
