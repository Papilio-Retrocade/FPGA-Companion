# FPGA-Companion v2.2.4

Fixes an out-of-bounds read in the network recovery status response by checking
the formatted length before sending it.

## Artifacts

- `fpga_companion-v2.2.4.bin` is the application for a loader-managed OTA slot.
  Installing it alone does not update the factory loader.
- `papilio-migration-v2.2.4-merged.bin` is a complete 4 MB provisioning image
  containing Companion and Papilio ESP Bootloader v0.1.3. It is for initial
  setup or migration, not ordinary updates: provisioning overwrites settings.
- The package also includes the partition table, initial OTA data, partition
  CSV, ZIP bundle, and SHA-256 checksums.

## Validation

- A build with the status fix was tested on a Retrocade: its status response
  had the expected length and contained no NUL or trailing bytes.
- Twenty MCP Wi-Fi FPGA uploads were acknowledged, with Companion subsequently
  identified and Splash matched in sampled HDMI frames. Status probes had
  intermittent timeouts; the test does not establish uninterrupted uptime or
  full-image readback.
- The exact v2.2.4 release images have not been hardware-provisioned.
