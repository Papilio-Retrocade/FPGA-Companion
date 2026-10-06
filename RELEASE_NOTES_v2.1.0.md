# FPGA-Companion v2.1.0

This release adds persistent root-folder ROM loading for Papilio Retrocade
while preserving the existing `/roms` behavior for clients that do not pass
the new option.

## Added

- `POST /rom-load?name=<filename>&location=root` saves the uploaded image in
  the SD-card root and can insert it into the running core. The default
  `location=roms` behavior remains unchanged.
- The Getting Started flow saves `a2600crt.bin` in the SD-card root, where the
  A2600 core can load it again after a reboot without an XML configuration
  file.

## Artifacts

- `fpga_companion-v2.1.0.bin`: application image for an inactive OTA slot
- `papilio-migration-v2.1.0-merged.bin`: complete migration image for boards
  that need the Papilio ESP Bootloader installed
- `partitions_loader.csv`: 4 MB loader layout

## Validation

- ESP-IDF 6.0.1 ESP32-S3 release build
- Papilio Retrocade board update through the factory loader's USB-serial app
  update path
- Getting Started ROM upload and reboot persistence confirmed on hardware
