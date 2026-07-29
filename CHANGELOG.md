# Changelog

All notable changes to the Papilio Retrocade fork of FPGA-Companion are documented here.

This fork adds ESP32-S3 support for the [Papilio Retrocade](https://papilioworks.com) board
(GW2A-18C FPGA + ESP32-S3) on top of Till Harbaum's upstream
[FPGA-Companion](https://github.com/harbaum/FPGA-Companion).

## [1.0.0] - 2026-05-21

First stable release for the Papilio Retrocade. All features below are additions
to upstream FPGA-Companion as of `6c7b09e` (tape fix #29).

### Added

#### FPGA programming & OTA updates
- FPGA bitstream loading from SD card to SPI flash (`Load FPGA Bitfiles to Flash`)
- FPGA JTAG SRAM programming driven by the ESP32 (`jtag_gowin.c`), with a
  streaming API, correct bit ordering, and cold-boot reset suppression
- HTTP OTA server on port 3232:
  - `/` — status page, `/update` — ESP32 firmware OTA update
  - `/fpga-update` — write an FPGA bitstream to SPI flash and reconfigure,
    optimized 27× (62 s → 2.3 s)
  - `/fpga-jtag-sram` — load a bitstream directly into FPGA SRAM via JTAG
  - `/fpga-recover` — recovery when the flash bitstream area is blank
  - `/flash-write?addr=0xNNNNNN` — raw SPI flash writes (≥ 0x200000;
    the bitstream region is protected)
  - FPGA OTA pre-loads a bootloader into SRAM before flash writes
  - JTAG retry logic and GPIO optimization
- See `src/esp32/OTA_REFERENCE.md` and `src/esp32/JTAG_PROGRAMMING.md`

#### Bluetooth & input
- BLE HID host support (`bt_hid.c`, Bluedroid `esp_hidh`): wireless gamepads
  connect, pair, bond, and reconnect
  - Xbox Series controllers (16-bit axis scaling, HAT/D-pad decode, correct
    OSD open/select buttons)
  - PS5 DualSense, PS4, and DualShock 3 controllers
- Bluetooth scan tuning: periodic scans every 10 s, scan cancel when a
  device connects, reduced verbose logging

#### Cores
- SNES core support — `core_snes`, `config/snes.xml`, `CORE_ID_SNES = 0x06` (SNEStang)
- NES core support — `core_nes`, `config/nes.xml`, `CORE_ID_NES = 0x07` (NEStang)
- C64 fixes: keyboard encoding, joyport OSD order, SDC copier headers, boot reset

#### Diagnostics
- WiFi UDP log streaming on port 7777 (works around the ESP32-S3 USB OTG
  conflict when USB is in host mode) with status LED indication

### Security
- `sdkconfig` is no longer tracked; WiFi credentials live in the gitignored
  `sdkconfig.defaults.local` (see `sdkconfig.defaults.local.example`)

### Notes
- Release binaries are built **without** WiFi credentials. To use WiFi UDP
  logging or OTA, build from source with your own `sdkconfig.defaults.local`.
- NES OSD ROM loading (streaming ROMs from the SD card via the OSD menu) is
  still experimental and lives on the `retrocade_v04` / `retrocade-spi-stream`
  branches; it is not part of this release.

[1.0.0]: https://github.com/Papilio-Retrocade/FPGA-Companion/releases/tag/v1.0.0
