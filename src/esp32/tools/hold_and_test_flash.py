"""Combined USB_HOST_HOLD + FPGA_FLASH_BEGIN bench test on a single serial connection.

Opening a *new* pyserial connection to this board can spuriously toggle the
board into ROM bootloader/download mode (classic ESP32 auto-reset circuit
side effect of DTR/RTS being asserted during OS port init). Running
hold_usb_host.py and test_serial_flash.py as two separate script invocations
means the port gets opened twice, doubling the chance of a spurious reset
between "hold" and "flash". This script does both over one connection.

Usage: python hold_and_test_flash.py COM13 sram path\\to\\bitstream.bin
"""
import sys
import time

import serial

PORT = sys.argv[1]
TARGET = sys.argv[2]
FILE = sys.argv[3]

with open(FILE, "rb") as f:
    data = f.read()
size = len(data)
print(f"Bitstream: {FILE} ({size} bytes), target={TARGET}", flush=True)

print(f"Waiting for {PORT} to become available...", flush=True)
ser = None
deadline = time.time() + 60
while time.time() < deadline:
    try:
        # Pre-set DTR/RTS *before* the OS actually opens the port. The
        # ESP32-S3's native USB-Serial/JTAG peripheral resets the chip the
        # instant DTR goes active (silicon feature, not an external RC
        # circuit) and RTS selects download-vs-normal boot at that instant.
        # Setting these only *after* Serial() returns is too late - pyserial
        # (and the Windows CDC driver) already assert both lines as part of
        # the open() call itself, which is enough to flip the board into ROM
        # download mode. Constructing with port=None defers the actual open
        # until we've forced both lines low.
        candidate = serial.Serial()
        candidate.port = PORT
        candidate.baudrate = 115200
        candidate.timeout = 0.2
        candidate.dtr = False
        candidate.rts = False
        candidate.open()
        ser = candidate
        break
    except Exception:
        time.sleep(0.05)

if ser is None:
    print("Timed out waiting for port.", flush=True)
    sys.exit(1)

print("Port opened, sending USB_HOST_HOLD immediately.", flush=True)
ser.write(b"USB_HOST_HOLD\n")
ser.flush()

leftover = b""

def next_line(timeout=10):
    global leftover
    deadline = time.time() + timeout
    buf = leftover
    while time.time() < deadline:
        if b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            leftover = buf
            return line.decode(errors="replace").rstrip("\r")
        chunk = ser.read(256)
        if chunk:
            buf += chunk
    leftover = buf
    return None

# Drain any hold-ack / boot log lines for a couple seconds.
hold_deadline = time.time() + 2
while time.time() < hold_deadline:
    line = next_line(timeout=0.3)
    if line is not None:
        print(f"<< {line}", flush=True)

cmd = f"FPGA_FLASH_BEGIN {TARGET} {size}\n".encode()
print(f">> {cmd!r}", flush=True)
ser.write(cmd)
ser.flush()

start = time.time()
ready_line = None
while time.time() - start < 10:
    line = next_line(timeout=1)
    if line is None:
        continue
    print(f"<< {line}", flush=True)
    if line.strip() == "READY" or line.startswith("ERROR"):
        ready_line = line.strip()
        break

if ready_line != "READY":
    print(f"FAILED to get READY (got: {ready_line!r})", flush=True)
    sys.exit(1)

print("Got READY, streaming bitstream bytes...", flush=True)
t0 = time.time()
CHUNK = 4096
written = 0
while written < size:
    end = min(written + CHUNK, size)
    ser.write(data[written:end])
    written = end
ser.flush()
print(f"Wrote {written} bytes in {time.time()-t0:.2f}s, waiting for result...", flush=True)

final = None
start = time.time()
while time.time() - start < 180:
    line = next_line(timeout=2)
    if line is None:
        continue
    print(f"<< {line}", flush=True)
    if line.startswith("FPGA_FLASH_OK") or line.startswith("FPGA_FLASH_ERROR"):
        final = line
        break

elapsed = time.time() - t0
if final and final.startswith("FPGA_FLASH_OK"):
    print(f"SUCCESS in {elapsed:.2f}s total", flush=True)
else:
    print(f"FAILED: {final!r}", flush=True)
    sys.exit(1)
