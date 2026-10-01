#!/usr/bin/env python3
"""
Interactive keyboard passthrough to a Papilio Retrocade board's OTA /type
endpoint. Lets you drive the running core (and its OSD menu) live from your
PC keyboard, without a physical USB/BLE keyboard attached to the board.

Usage:
    python ota_keyboard_passthrough.py <device-ip>

Controls:
    F12          toggle the OSD menu open/closed (same as a real F12 key)
    Up/Down      navigate the OSD menu (when open)
    Left/Right   navigate the OSD menu (when open)
    PageUp/Dn    fast scroll the OSD menu (when open)
    Enter/Space  select the highlighted OSD entry when the menu is open,
                 otherwise typed normally at the BASIC/core prompt
    Tab          always selects the highlighted OSD entry (explicit alias)
    Backspace    typed at the BASIC/core prompt (C64 DEL)
    (anything else) typed at the BASIC/core prompt
    Esc          quit this script

Windows only (uses msvcrt for raw key reads). Each keystroke is sent as its
own HTTP request, so latency is bound by round-trip time to the board.
"""
import sys
import msvcrt
import urllib.request

# Scan codes returned as the second byte after a 0x00/0xE0 prefix byte.
SCAN_UP, SCAN_DOWN, SCAN_LEFT, SCAN_RIGHT = 72, 80, 75, 77
SCAN_PGUP, SCAN_PGDN = 73, 81
SCAN_F12 = 134  # some consoles report 0x86 (134); others report 0x87 (135)


def send(ip, body):
    url = f"http://{ip}:3232/type"
    req = urllib.request.Request(url, data=body.encode("ascii", "ignore"), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            resp.read()
    except Exception as e:
        print(f"\n[send error: {e}]", end="", flush=True)


def main():
    if len(sys.argv) != 2:
        print("Usage: python ota_keyboard_passthrough.py <device-ip>")
        sys.exit(1)
    ip = sys.argv[1]

    print(f"Connected to {ip}:3232/type -- F12 toggles OSD, Esc quits.")
    print("Enter/Space select the OSD entry when it's open, else typed normally. Tab always selects.\n")

    osd_open = False
    send(ip, "{hide}")

    while True:
        ch = msvcrt.getch()

        if ch in (b"\x00", b"\xe0"):
            scan = msvcrt.getch()[0]
            if scan == SCAN_F12:
                osd_open = not osd_open
                send(ip, "{show}" if osd_open else "{hide}")
            elif scan == SCAN_UP:
                send(ip, "{up}")
            elif scan == SCAN_DOWN:
                send(ip, "{down}")
            elif scan == SCAN_LEFT:
                send(ip, "{left}")
            elif scan == SCAN_RIGHT:
                send(ip, "{right}")
            elif scan == SCAN_PGUP:
                send(ip, "{pgup}")
            elif scan == SCAN_PGDN:
                send(ip, "{pgdn}")
            continue

        if ch == b"\x1b":  # Esc quits the script
            print("\nExiting.")
            break

        if ch == b"\t":  # Tab always selects the highlighted OSD entry
            send(ip, "{select}")
            continue

        if ch == b"\r":
            send(ip, "{select}" if osd_open else "{enter}")
            continue

        if ch == b" ":
            send(ip, "{select}" if osd_open else "{space}")
            continue

        try:
            send(ip, ch.decode("ascii"))
        except UnicodeDecodeError:
            pass  # ignore non-ASCII input


if __name__ == "__main__":
    main()
