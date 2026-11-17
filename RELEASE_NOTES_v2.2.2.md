# FPGA-Companion v2.2.2

This patch release fixes FPGA-Companion failing to start after a new FPGA
core (for example A2600 or C64) is flashed through the Papilio ESP Bootloader.

## Fixed

- After the bootloader flashed a core and resumed the Companion, the Companion
  could crash with an interrupt watchdog timeout during SPI start-up, and the
  bootloader then rolled the board back to itself. `esp_restart()` keeps the
  previous run's FPGA IRQ (GPIO9) interrupt enable, and a freshly booted core
  holds IRQ low, so the GPIO interrupt service looped before the IRQ handler
  was attached. The IRQ interrupt is now disabled and configured before the
  service is installed.

## Artifacts

- `fpga_companion-v2.2.2.bin`: application image for an inactive OTA slot
- `papilio-migration-v2.2.2-merged.bin`: migration image with Papilio ESP
  Bootloader v0.1.1 and FPGA-Companion v2.2.2
- `partitions_loader.csv`: 4 MB loader layout

## Validation

- ESP-IDF 6.0.1 ESP32-S3 release build; app descriptor reports v2.2.2.
- On a Papilio Retrocade, v2.2.1 crashed and rolled back on the first
  bootloader core flash. The fix completed 6 of 6 core-flash cycles,
  alternating C64 and A2600 (return to bootloader, `/fpga-update`, `/resume`),
  with the Companion detecting the core and reaching its main loop each time.
