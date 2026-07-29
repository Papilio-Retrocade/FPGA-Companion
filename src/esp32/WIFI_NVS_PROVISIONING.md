# WiFi Credential Provisioning via NVS (no rebuild required)

## Overview

FPGA-Companion normally gets its WiFi SSID/password from `sdkconfig.defaults.local`
at **compile time** (`CONFIG_WIFI_LOG_SSID` / `CONFIG_WIFI_LOG_PASSWORD`).

The firmware now also checks the device's NVS partition for a runtime override
before falling back to those compiled-in values, using namespace `wifi_cfg`:

| Key | Type | Description |
|---|---|---|
| `ssid` | string | Network SSID (max 32 chars) |
| `pass` | string | Network password (max 63 chars) |

If neither key is present in NVS, behavior is unchanged — the firmware uses
`CONFIG_WIFI_LOG_SSID`/`CONFIG_WIFI_LOG_PASSWORD` exactly as before.

This means you can set (or change) WiFi credentials on an **already-flashed**
device using only `esptool` and Espressif's official `nvs_partition_gen`
utility — no ESP-IDF toolchain, no firmware rebuild, and it survives future
OTA/USB application updates (the app partition and the NVS partition are
separate flash regions).

## Requirements

```bash
pip install esptool esp-idf-nvs-partition-gen
```

Both are official Espressif packages on PyPI and work on Windows, macOS, and Linux.

## Step 1: Create the credentials CSV

Create `wifi_nvs.csv`:

```csv
key,type,encoding,value
wifi_cfg,namespace,,
ssid,data,string,YourNetworkName
pass,data,string,YourNetworkPassword
```

## Step 2: Generate the NVS binary

This board's `nvs` partition is 20 KB (`0x5000` bytes) — see `partitions_ota.csv`.
The size argument below must match that exactly:

```bash
python -m esp_idf_nvs_partition_gen generate wifi_nvs.csv wifi_nvs.bin 0x5000
```

## Step 3: Flash it with esptool

The `nvs` partition sits at offset `0x9000` in the 4 MB flash layout used by
`partitions_ota.csv`:

```bash
python -m esptool --chip esp32s3 -b 460800 write-flash 0x9000 wifi_nvs.bin
```

Power-cycle the device — it connects using the new credentials. Application
firmware updates (OTA or USB, at `0x10000`/`0x200000`) do **not** touch the
`nvs` partition, so credentials persist across updates.

## Reverting to the compiled-in credentials

Erase just the NVS partition and reboot:

```bash
python -m esptool --chip esp32s3 erase_region 0x9000 0x5000
```

## Caution

Erasing or overwriting the `nvs` partition also clears any other runtime state
FPGA-Companion or the WiFi driver has stored there (e.g. WiFi driver
calibration data written internally by `esp_wifi`). This is expected and safe
— everything is regenerated on next boot — but don't use this technique to
patch a *running* device's `nvs` partition for unrelated purposes without
checking what else lives in it.
