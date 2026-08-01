"""Poll COM13 as fast as possible and send USB_HOST_HOLD the instant it enumerates.

The board only exposes its USB-Serial/JTAG console for a fixed 5s grace window
after boot (see mcu_hw.c USB_HOST_GRACE_MS) before switching the native USB PHY
to Host mode, at which point the port disappears. This script is meant to be
started *before* pressing the board's RESET button so it's already spinning
in a tight retry loop when the port appears.
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM13"
BAUD = 115200

print(f"Waiting for {PORT} to become available...", flush=True)
ser = None
deadline = time.time() + 60
while time.time() < deadline:
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.2)
        break
    except Exception:
        time.sleep(0.05)

if ser is None:
    print("Timed out waiting for port.", flush=True)
    sys.exit(1)

print(f"Port opened, sending USB_HOST_HOLD immediately.", flush=True)
ser.write(b"USB_HOST_HOLD\n")
ser.flush()

end = time.time() + 12
buf = b""
while time.time() < end:
    try:
        chunk = ser.read(256)
    except Exception as e:
        print(f"read error: {e}", flush=True)
        break
    if chunk:
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(line.decode(errors="replace").rstrip("\r"), flush=True)

print("Done.", flush=True)
ser.close()
