# FPGA Companion OTA Firmware Update Reference

## Overview

The device runs an HTTP OTA server on port **3232** whenever it connects to WiFi.
New firmware is uploaded with a single `curl` command — no USB cable, no serial flasher.

OTA supports **two update types**:
- **ESP32 firmware**: Dual-partition scheme with automatic rollback on failure
- **FPGA bitstreams**: Direct flash write to 0x100000 with automatic reconfiguration

### Browser access (hosted web flasher)

Every endpoint responds to the `OPTIONS` preflight and sets
`Access-Control-Allow-Origin` / `Access-Control-Allow-Private-Network: true`,
so a browser page (e.g. papilioworks.com/flash) can `fetch()` these endpoints
directly from JavaScript instead of needing a local agent. Configure the
allowed origin with `CONFIG_OTA_CORS_ALLOW_ORIGIN` (default `"*"`).

WiFi credentials can also be set over the same USB serial port used to flash
the ESP32 firmware — send `WIFI_SSID=<ssid>\n` then `WIFI_PASS=<pass>\n`
before the device reconnects; see `wifi_provision.h`.

---

## Quick Reference

### Upload ESP32 firmware
```
curl -X POST http://<device-ip>:3232/update --data-binary @build/fpga_companion.bin
```

### Upload FPGA bitstream
```
curl -X POST http://<device-ip>:3232/fpga-update --data-binary @your_bitstream.bin
```

### Check device status / current version
```
curl http://<device-ip>:3232/
```

### Known device IP (lab unit)
```
10.0.4.94
```
So the full upload command for the lab unit is:
```
curl -X POST http://10.0.4.94:3232/update --data-binary @build/fpga_companion.bin
```

**FPGA bitstream:**
```
curl -X POST http://10.0.4.94:3232/fpga-update --data-binary @a2600nano_retrocade.bin
```

---

## FPGA OTA Update Mechanism

### How It Works

1. **ESP32 receives bitstream** via HTTP POST on `/fpga-update` endpoint
2. **FPGA held in reset** - ESP32 pulls RECONFIG_N (GPIO 13) LOW to ensure:
   - FPGA stops accessing flash (no bus conflicts)
   - SPI passthrough pins remain connected in user cores (A2600, etc.)
3. **Flash erased** - 2 MB region @ 0x100000
4. **Bitstream written** - 4KB buffer for optimal performance
5. **FPGA released** - RECONFIG_N goes HIGH
6. **Auto-reconfiguration** - FPGA boots from new bitstream @ 0x100000

### Performance Improvements

- **Old SD card method**: 256-byte buffer, ~60 seconds for 2MB bitstream
- **New OTA method**: 4096-byte buffer, **~7-10 seconds for 2MB**
- **16x faster** due to larger buffer and optimized flash writes

### Safety Features

- **Semaphore locking**: Prevents concurrent SPI bus access
- **FPGA reset coordination**: Ensures no flash conflicts during programming
- **Clean erase**: Full region cleared before writing
- **Progress logging**: Real-time status every 64KB

### Requirements

- **WiFi connected**: OTA server must be running
- **Compatible FPGA core**: User cores must have SPI passthrough (A2600, etc.)
- **Multiboot enabled**: Bitstream at 0x000000 must support multiboot to 0x100000

---

## ESP32 Firmware OTA Details

ESP32 firmware uses a dual-partition scheme:
- Running slot is never touched
- New image written to inactive slot  
- Device reboots into new image
- Automatic rollback on boot failure

---

## Prerequisites

| Requirement | Detail |
|---|---|
| WiFi connected | OTA server only starts when WiFi connects successfully |
| Build complete | `idf.py build` must succeed first — binary is at `build/fpga_companion.bin` |
| Port open | Port 3232 TCP must be reachable (same LAN is fine; no firewall rule needed on most routers) |

---

## Finding the Device IP

The IP is logged when WiFi connects. Capture it with the UDP log listener:

**Windows (ncat/nmap):**
```
ncat -u -l 7777
```

**Linux / macOS:**
```
nc -u -l -p 7777
```

Look for a line like:
```
I (3421) wifi_log: got ip: 10.0.4.94
```

Alternatively, check your router's DHCP client list for a device named `FPGA-Companion` or `Espressif`.

---

## Full Build → Flash Workflow

Run from `C:\development\FPGA-Companion\src\esp32`:

```powershell
# 1. Build
idf.py build

# 2. Upload over the air
curl -X POST http://10.0.4.94:3232/update --data-binary @build/fpga_companion.bin
```

The device reboots automatically after a successful upload (~2–5 seconds after `curl` returns).

---

## Status Response

`GET http://<device-ip>:3232/` returns plain text like:

```
FPGA Companion OTA Server
=========================
Running partition : ota_0
Firmware version  : 0.1.0
Build date        : May 11 2026 14:32:07

To upload new firmware:
  curl -X POST http://<device-ip>:3232/update --data-binary @build/fpga_companion.bin
```

---

## Configuration (sdkconfig.defaults)

| Key | Default | Description |
|---|---|---|
| `CONFIG_OTA_ENABLE` | `y` | Enable OTA server (disable to save ~20 KB flash) |
| `CONFIG_OTA_PORT` | `3232` | HTTP server port |
| `CONFIG_WIFI_LOG_ENABLE` | `y` | Required — OTA server only starts if WiFi is enabled |
| `CONFIG_WIFI_LOG_SSID` | `"YourSSID"` | Set in `sdkconfig.defaults.local` |
| `CONFIG_WIFI_LOG_PASSWORD` | `"YourPassword"` | Set in `sdkconfig.defaults.local` |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions_ota.csv` | Dual-slot OTA partition table |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `"4MB"` | Match your actual flash chip |

WiFi credentials live in `sdkconfig.defaults.local` (gitignored). Copy the example:
```
sdkconfig.defaults.local.example  →  sdkconfig.defaults.local
```

---

## First-Time USB Flash

OTA requires the OTA partition table to already be on the device.
On a brand-new board, flash once over USB:

```powershell
idf.py flash
```

After that, all subsequent updates can use OTA.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `curl: (7) Failed to connect` | Device not on WiFi or wrong IP | Check UDP log for current IP |
| `curl: (52) Empty reply` | OTA not enabled or WiFi not connected yet | Wait for boot to complete (~5 s), check `sdkconfig.defaults` |
| Upload succeeds but device doesn't boot new firmware | Bad binary / wrong target | Ensure `idf.py build` completed without errors; device rolls back automatically |
| `400 Bad Request` | Empty POST body | Ensure `@build/fpga_companion.bin` path is correct and file exists |
