"""Bench-test the FPGA_FLASH_BEGIN serial protocol against real hardware.

Usage: python test_serial_flash.py COM13 sram path\\to\\bitstream.bin
"""
import sys
import time

import serial

PORT = sys.argv[1]
TARGET = sys.argv[2]  # "flash" or "sram"
FILE = sys.argv[3]

with open(FILE, "rb") as f:
    data = f.read()
size = len(data)
print(f"Bitstream: {FILE} ({size} bytes), target={TARGET}", flush=True)

ser = serial.Serial(PORT, 115200, timeout=1)
time.sleep(0.2)
ser.reset_input_buffer()

cmd = f"FPGA_FLASH_BEGIN {TARGET} {size}\n".encode()
print(f">> {cmd!r}", flush=True)
ser.write(cmd)
ser.flush()

def read_line(timeout=10):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if b"\n" in buf:
                line, rest = buf.split(b"\n", 1)
                ser.timeout = 0.05
                # push back the rest by re-reading isn't possible; just return and track leftover
                return line.decode(errors="replace").rstrip("\r"), rest
    return None, buf

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

# Wait for READY / ERROR. For target=flash the board does bootloader-SRAM
# load + SPI init + full-region erase *before* replying READY, which can
# take up to ~60s worst case - keep this generous.
start = time.time()
ready_line = None
while time.time() - start < 90:
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
    ser.flush()
    written = end

    # Per-chunk flow control: wait for the device's PROGRESS ack before
    # sending more. The device's usb_serial_jtag RX ring buffer is small
    # (16 KB) - without this, the whole payload gets handed to the OS/USB
    # stack immediately and can overrun that buffer, wedging the USB
    # transport permanently if a flash write is ever slower than the
    # incoming byte rate.
    acked = False
    ack_deadline = time.time() + 10
    while time.time() < ack_deadline:
        line = next_line(timeout=1)
        if line is None:
            continue
        print(f"<< {line}", flush=True)
        if line.startswith("FPGA_FLASH_ERROR"):
            print(f"FAILED: {line!r}", flush=True)
            sys.exit(1)
        m = line.startswith("PROGRESS")
        if m and int(line.split()[1]) >= written:
            acked = True
            break
    if not acked:
        print(f"FAILED: no PROGRESS ack after {written} bytes", flush=True)
        sys.exit(1)
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
