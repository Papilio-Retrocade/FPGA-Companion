/* tangcore.h — TangCore BL616-compatible UART protocol for ESP32 FPGA-Companion
 *
 * Frame format: 0xAA len_hi len_lo cmd [payload]
 * Baud rate: 2 Mbps, 8N1, no flow control
 * FPGA UART RX = GPIO43 (TX) → FPGA pin E14
 * FPGA UART TX = GPIO44 (RX) ← FPGA pin C9
 */

#ifndef TANGCORE_H
#define TANGCORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Commands: ESP32 → FPGA ---- */
#define TANGCORE_SYNC_BYTE          0xAAu
#define TANGCORE_CMD_QUERY_CORE     0x01u  /* get core ID */
#define TANGCORE_CMD_SET_CONFIG     0x03u  /* set 32-bit core config */
#define TANGCORE_CMD_MOVE_CURSOR    0x04u  /* move OSD text cursor (x, y) */
#define TANGCORE_CMD_WRITE_STRING   0x05u  /* write string from cursor (payload = string) */
#define TANGCORE_CMD_SET_LOADING    0x06u  /* set loading state (0 = done, 1 = loading) */
#define TANGCORE_CMD_LOAD_ROM       0x07u  /* load ROM data block */
#define TANGCORE_CMD_OSD_ENABLE     0x08u  /* OSD on/off (payload[0] & 1) */
#define TANGCORE_CMD_JOYSTICK       0x09u  /* USB joystick state hid1[15:0] hid2[15:0] */

/* ---- Responses: FPGA → ESP32 ---- */
#define TANGCORE_RESP_CORE_ID       0x01u  /* core ID byte */
#define TANGCORE_RESP_JOY_STATE     0x03u  /* DS2/SNES joypad state every 20ms (4 bytes) */

/* ---- Core IDs ---- */
#define TANGCORE_CORE_NES           1u
#define TANGCORE_CORE_SNES          2u

/* ---- HID bit layout (16-bit SNES/NES format) ----
 * Matches iosys_bl616 hid1/hid2 input: (R L X A RT LT DN UP START SELECT Y B)
 * MSB to LSB = bit[11] to bit[0]
 */
#define TANGCORE_BTN_B              (1u <<  0)
#define TANGCORE_BTN_Y              (1u <<  1)
#define TANGCORE_BTN_SELECT         (1u <<  2)
#define TANGCORE_BTN_START          (1u <<  3)
#define TANGCORE_BTN_UP             (1u <<  4)
#define TANGCORE_BTN_DOWN           (1u <<  5)
#define TANGCORE_BTN_LEFT           (1u <<  6)
#define TANGCORE_BTN_RIGHT          (1u <<  7)
#define TANGCORE_BTN_A              (1u <<  8)
#define TANGCORE_BTN_X              (1u <<  9)
#define TANGCORE_BTN_L              (1u << 10)
#define TANGCORE_BTN_R              (1u << 11)

/* Active interface flag (shared with main.c / com_task):
 *   0 = undecided (both tasks probing)
 *   1 = SPI iosys_retrocade (com_task won)
 *   2 = UART iosys_bl616   (tangcore_task won)
 */
extern volatile int active_interface;

/* Update joystick state to be forwarded to the FPGA via UART command 0x09.
 * Called by hid.c whenever a USB joystick changes state.
 * player = 0 (joy1) or 1 (joy2).
 * buttons = 16-bit SNES-format button bitfield.
 */
void tangcore_set_joy(int player, uint16_t buttons);

/* FreeRTOS task entry point.  Launched by app_main alongside com_task. */
void tangcore_task(void *p);

#endif /* TANGCORE_H */
