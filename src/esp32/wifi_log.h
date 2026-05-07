#ifndef WIFI_LOG_H
#define WIFI_LOG_H

/*
 * wifi_log.h - UDP wireless logging for FPGA Companion
 *
 * Connects to a WiFi network in station mode, then redirects all printf()
 * output to UDP broadcast packets on port 7777.
 *
 * On your PC run:
 *   Windows:  ncat -u -l 7777
 *   Linux:    nc -u -l -p 7777
 *   Python:   python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('',7777));
 *              [print(s.recvfrom(4096)[0].decode()) for _ in iter(int,1)]"
 *
 * WiFi credentials are set in sdkconfig.defaults (or idf.py menuconfig):
 *   CONFIG_WIFI_LOG_SSID
 *   CONFIG_WIFI_LOG_PASSWORD
 *
 * To disable wireless logging entirely, do not define ENABLE_WIFI_LOG.
 */

#include "sdkconfig.h"
#ifdef CONFIG_WIFI_LOG_ENABLE

/**
 * Install the vprintf hook and ring buffer immediately at boot so no log
 * lines are missed. Call this as early as possible in mcu_hw_init(),
 * before any other output. Does not start WiFi.
 */
void wifi_log_early_init(void);

/**
 * Start WiFi, wait for connection, open UDP socket, flush the ring buffer,
 * then begin live UDP forwarding. Call after wifi_log_early_init().
 * Blocks for up to 15 seconds waiting for WiFi connection.
 */
void wifi_log_init(void);

#else
static inline void wifi_log_early_init(void) {}
static inline void wifi_log_init(void) {}
#endif // CONFIG_WIFI_LOG_ENABLE

#endif // WIFI_LOG_H
