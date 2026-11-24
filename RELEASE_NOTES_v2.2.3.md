# FPGA-Companion v2.2.3

Updates the migration package to include Papilio ESP Bootloader v0.1.3.
Companion runtime behavior is unchanged from v2.2.2.

## Fixed

- Provisioning no longer installs bootloader v0.1.1. The bundled v0.1.3
  rounds FPGA flash erases up to complete sectors and stops USB/HTTP uploads
  on erase/write errors. Previously USB uploads could report success while
  leaving an unbootable FPGA core.
- The rebuilt Companion app reports v2.2.3 to match this release.

## Artifacts

- `fpga_companion-v2.2.3.bin`: raw user application for a loader-managed OTA slot.
  Installing this file alone does **not** update the factory loader.
- `papilio-migration-v2.2.3-merged.bin`: provisioning image with bootloader
  v0.1.3 and Companion v2.2.3. Intended for initial setup or migration, not
  ordinary updates to an already-configured board.
- `partition-table.bin`, `ota_data_initial.bin`, and `partitions_loader.csv`:
  the unchanged 4 MB factory/OTA layout.
- ZIP bundle and SHA-256 checksums.

## Existing boards

For boards already using the compatible factory/OTA layout, back up and update
only the factory loader at `0x20000` using the
[bootloader v0.1.3 release](https://github.com/Papilio-Labs/papilio-esp-bootloader/releases/tag/v0.1.3).
Do not erase the whole ESP flash or overwrite settings/OTA metadata merely to
apply that fix. Companion application updates continue through the loader.

## Validation

- ESP-IDF 6.0.1 ESP32-S3 release build with a v2.2.3 app descriptor.
- Migration image components verified against their inputs at the documented
  offsets; release images checked for private WiFi credentials.
- Bootloader v0.1.3 changes were hardware-tested with Companion v2.2.2:
  USB A2600-to-C64 and WiFi C64-to-A2600 updates completed, and HDMI was
  confirmed for both cores. FPGA uploads were acknowledged, not verified by
  full-image readback.
- Full provisioning of the new merged image has not been hardware-tested.
