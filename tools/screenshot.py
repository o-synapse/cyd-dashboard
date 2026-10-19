"""Grab a screenshot from the running dashboard over USB serial.

Usage: python tools/screenshot.py /dev/cu.usbserial-XXXX out.png [hour]
  hour: optional two-digit hour to preview (e.g. 07), or -- for real time
"""
import sys
import time
import serial
from PIL import Image

W, H = 320, 240
port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(None, 115200, timeout=20)
s.port = port
s.dtr = False  # set before opening so the board isn't reset
s.rts = False
s.open()
time.sleep(0.3)
s.reset_input_buffer()
if len(sys.argv) > 3:
    s.write(b"h" + sys.argv[3].encode()[:2])
    time.sleep(1.5)
    s.reset_input_buffer()
s.write(sys.argv[4].encode() if len(sys.argv) > 4 else b"")
time.sleep(0.2)
s.write(b"S")
while True:
    line = s.readline()
    if not line:
        sys.exit("timed out waiting for screenshot")
    if b"SCREENSHOT_BEGIN" in line:
        break
data = s.read(W * H * 2)
img = Image.new("RGB", (W, H))
px = img.load()
for i in range(W * H):
    v = (data[2 * i] << 8) | data[2 * i + 1]
    px[i % W, i // W] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
img.resize((W * 2, H * 2), Image.LANCZOS).save(out)
print("saved", out)
