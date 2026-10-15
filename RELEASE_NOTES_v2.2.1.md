# FPGA-Companion v2.2.1

This patch release corrects the embedded firmware version metadata in the
v2.2.0 release image. The v2.2.0 application was built with a stale ESP-IDF
`PROJECT_VER` and reported itself as v2.0.0. This release is rebuilt from a
fresh configuration and reports v2.2.1.

The OSD connectivity and input-aware visibility features from v2.2.0 are
unchanged.

## Artifacts

- `fpga_companion-v2.2.1.bin`: application image for an inactive OTA slot
- `papilio-migration-v2.2.1-merged.bin`: migration image with Papilio ESP
  Bootloader v0.1.1 and FPGA-Companion v2.2.1
- `partitions_loader.csv`: 4 MB loader layout

## Validation

- ESP-IDF 6.0.1 ESP32-S3 release build.
- Verified the app descriptor reports v2.2.1 before packaging.
