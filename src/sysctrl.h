/*
  sysctrl.h

  MiSTeryNano system control interface
*/

#ifndef SYS_CTRL_H
#define SYS_CTRL_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "spi.h"

// the MCU can adopt to the core running to e.g.
// change the menu and the keyboard mapping
#define CORE_ID_UNKNOWN  0x00
#define CORE_ID_ATARI_ST 0x01
#define CORE_ID_C64      0x02
#define CORE_ID_VIC20    0x03
#define CORE_ID_AMIGA    0x04
#define CORE_ID_ATARI_2600    0x05
#define CORE_ID_SNES          0x06
#define CORE_ID_NES           0x07

extern unsigned char core_id;

int  sys_status_is_valid(void);
void sys_set_leds(char);
void sys_set_rgb(unsigned long);
unsigned char sys_get_buttons(void);
void sys_set_val(char, uint8_t);
unsigned char sys_irq_ctrl(unsigned char);
void sys_handle_interrupts(unsigned char);
bool sys_wait4fpga(void);

// Suppress the MCU auto-reset that normally fires when the FPGA reports a
// cold-boot event. Used when we deliberately reload the FPGA via JTAG SRAM
// programming with a bitstream that may not implement the MiSTeryNano SPI
// protocol — in that case the "cold boot" detection reads garbage and would
// otherwise reboot the MCU and reload the SPI-flash bootloader bitstream.
void sys_set_suppress_reset(bool suppress);

void sys_run_action(config_action_t *);
void sys_run_action_by_name(char *);
const char *sys_get_config_name(void);

#endif // SYS_CTRL_H
