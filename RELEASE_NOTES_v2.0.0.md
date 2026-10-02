# FPGA-Companion v2.0.0

This release is the first stripped Papilio Retrocade Companion release. The
ESP32-S3 image is designed for the `ota_0`/`ota_1` application slots managed by
the Papilio ESP Bootloader.

## Breaking change

This image no longer contains the standalone flashing and recovery services
from the pre-Phase-6 releases such as `v1.1.1`. Install
`papilio-esp-bootloader v0.1.0` in the factory partition first. Existing
boards with the old partition layout require the one-time
`papilio-migration-v2.0.0-merged.bin` USB migration image.

## Artifacts

- `fpga_companion-v2.0.0.bin`: application image for an inactive OTA slot
- `papilio-migration-v2.0.0-merged.bin`: complete 4 MB migration image
- `partitions_loader.csv`: final 4 MB loader layout

The Companion build is `1,494,192` bytes (`0x16c8b0`) and fits the 1.5 MB OTA
slot with approximately 5% free space.

## Validation

The ESP-IDF 6.0.1 release build and host loader tests pass. Full migrated-board
regression across A2600, C64, and NES cores, interrupted-write recovery, and
fresh-versus-migrated hardware equivalence still require connected hardware.