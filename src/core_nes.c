/*
  core_nes.c

  NES specific menus and input mapping
*/

#include "core_nes.h"
#include "sdc.h"
#include "debug.h"

const char * core_nes_default_images[] = {
  NULL  // no default cartridge
};

static const char main_form_nes[] =
  "NEStang,;"                           // main form has no parent
  // --------
  "F,Cartridge:,0|NES;"                 // fileselector for ROM
  "S,System,1;"                         // System submenu is form 1
  "S,Storage,2;"                        // Storage submenu is form 2
  "S,Settings,3;"                       // Settings submenu is form 3
  "B,Reset,R;";                         // system reset

static const char system_form_nes[] =
  "System,0|2;"                         // return to form 0, entry 2
  // --------
  "L,Joyport 1:,USB #1 Joy|USB #2 Joy|Off,Q;"
  "L,Joyport 2:,USB #1 Joy|USB #2 Joy|Off,J;"
  "B,Reset,R;";

static const char storage_form_nes[] =
  "Storage,0|3;"                        // return to form 0, entry 3
  // --------
  "F,Cartridge:,0|NES;";               // fileselector

static const char settings_form_nes[] =
  "Settings,0|4;"                       // return to form 0, entry 4
  // --------
  "L,Scanlines:,None|25%|50%|75%,S;"
  "L,Volume:,Mute|33%|66%|100%,A;"
  "B,Save settings,S;";

const char *core_nes_forms[] = {
  main_form_nes,
  system_form_nes,
  storage_form_nes,
  settings_form_nes
};

// Q J S A
menu_legacy_variable_t core_nes_variables[] = {
  { 'Q', { 0 }},    // Joyport 1 = USB #1 Joy
  { 'J', { 2 }},    // Joyport 2 = Off
  { 'S', { 0 }},    // Scanlines = None
  { 'A', { 2 }},    // Volume = 66%
  { '\0',{ 0 }}
};

// NES uses gamepad only — no keyboard matrix
#define MISS (0)

const unsigned char core_nes_keymap[] = {
  MISS, // 00: NoEvent
  MISS, // 01: Overrun Error
  MISS, // 02: POST fail
  MISS, // 03: ErrorUndefined
  MISS, // 04: a
  MISS, // 05: b
  MISS, // 06: c
  MISS, // 07: d
  MISS, // 08: e
  MISS, // 09: f
  MISS, // 0a: g
  MISS, // 0b: h
  MISS, // 0c: i
  MISS, // 0d: j
  MISS, // 0e: k
  MISS, // 0f: l
  MISS, // 10: m
  MISS, // 11: n
  MISS, // 12: o
  MISS, // 13: p
  MISS, // 14: q
  MISS, // 15: r
  MISS, // 16: s
  MISS, // 17: t
  MISS, // 18: u
  MISS, // 19: v
  MISS, // 1a: w
  MISS, // 1b: x
  MISS, // 1c: y
  MISS, // 1d: z
  MISS, // 1e: 1
  MISS, // 1f: 2
  MISS, // 20: 3
  MISS, // 21: 4
  MISS, // 22: 5
  MISS, // 23: 6
  MISS, // 24: 7
  MISS, // 25: 8
  MISS, // 26: 9
  MISS, // 27: 0
  MISS, // 28: return
  MISS, // 29: escape
  MISS, // 2a: backspace
  MISS, // 2b: tab
  MISS, // 2c: space
  MISS, // 2d: -
  MISS, // 2e: =
  MISS, // 2f: [
  MISS, // 30: ]
  MISS, // 31: backslash
  MISS, // 32: EUR-1
  MISS, // 33: ;
  MISS, // 34: '
  MISS, // 35: `
  MISS, // 36: ,
  MISS, // 37: .
  MISS, // 38: /
  MISS, // 39: caps lock
  MISS, // 3a: F1
  MISS, // 3b: F2
  MISS, // 3c: F3
  MISS, // 3d: F4
  MISS, // 3e: F5
  MISS, // 3f: F6
  MISS, // 40: F7
  MISS, // 41: F8
  MISS, // 42: F9
  MISS, // 43: F10
  MISS, // 44: F11
  MISS, // 45: F12
  MISS, // 46: PrtScr
  MISS, // 47: scroll lock
  MISS, // 48: pause
  MISS, // 49: insert
  MISS, // 4a: home
  MISS, // 4b: page up
  MISS, // 4c: delete
  MISS, // 4d: end
  MISS, // 4e: page down
  MISS, // 4f: right
  MISS, // 50: left
  MISS, // 51: down
  MISS, // 52: up
  MISS, // 53: num lock
};

const unsigned char core_nes_modifier[] = {
  MISS, // 00: no modifier
  MISS, // 01: left ctrl
  MISS, // 02: left shift
  MISS, // 03: left ctrl + shift
  MISS, // 04: left alt
  MISS, // 05: left ctrl + alt
  MISS, // 06: left shift + alt
  MISS, // 07: left ctrl + shift + alt
  MISS, // 08: right ctrl
  MISS, // 09: right shift
  MISS, // 0a: right ctrl + shift
  MISS, // 0b: right alt
  MISS, // 0c: right ctrl + alt
  MISS, // 0d: right shift + alt
  MISS, // 0e: right ctrl + shift + alt
  MISS, // 0f: all modifiers
};
