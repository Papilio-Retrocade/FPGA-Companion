# FPGA-Companion v2.2.0

This release adds input-aware OSD visibility and live connectivity details to
the ESP32 OSD title bar.

## Added

- The two-line OSD title bar shows USB Host status, WiFi connection state, and
  the current IP address when connected.
- The OSD opens automatically when no supported keyboard or controller is
  connected, hides when one connects, and reopens when the last one disconnects.

## Artifacts

- `fpga_companion-v2.2.0.bin`: application image for an inactive OTA slot
- `papilio-migration-v2.2.0-merged.bin`: complete migration image for boards
  that need the Papilio ESP Bootloader installed
- `partitions_loader.csv`: 4 MB loader layout

## Validation

- ESP-IDF 6.0.1 ESP32-S3 release build
