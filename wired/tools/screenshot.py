#!/usr/bin/env python3
"""Save a PNG of what the wired NerdMiner's screen shows right now (LilyGo T-Display S3).

usage: screenshot.py [--port ...] [--scale 2] out.png

Pauses the relay, sends F, and the board reads its frame back from the display
controller over the parallel bus (3 bytes per pixel, 6 bits per colour), streams it as
"f <offset> <hex>" lines and ends with "f end <bytes>". The read-back is only reliable
at the stock 240 MHz, so the board is put on stock clock for the capture (P 8) and back on
the overclock from <state>/oc.json afterwards. The picture shows the last redraw, made at
full speed. Mining goes on meanwhile; the board only misses new work for a few seconds. No dependencies
beyond pyserial: the PNG is written with zlib.
"""
import argparse, os, struct, sys, time, zlib
from pathlib import Path
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "relay"))
import nerdport

W, H = 320, 170

ap = argparse.ArgumentParser()
ap.add_argument("--port", default=os.environ.get("NERD_PORT", "auto"), help="default auto (see nerdport.py)")
ap.add_argument("--baud", type=int, default=int(os.environ.get("NERD_BAUD", "921600")))
ap.add_argument("--state", default=os.environ.get("NERD_WIRED_STATE", str(Path.home() / ".local/state/nerd-wired")))
ap.add_argument("--pause", help="pause file the relay watches (default <state>/pause)")
ap.add_argument("--oc", help="overclock file to restore after the capture (default <state>/oc.json)")
ap.add_argument("--scale", type=int, default=1, help="integer upscale of the 320x170 image")
ap.add_argument("--bgr", action="store_true", help="swap red and blue (if the colours come out wrong)")
ap.add_argument("out")
a = ap.parse_args()
port = nerdport.resolve(a.port)

pause = Path(a.pause) if a.pause else Path(a.state) / "pause"
pause.parent.mkdir(parents=True, exist_ok=True)
pause.touch()
time.sleep(1.5)   # let the relay let go of the port
import json
oc = Path(a.oc) if a.oc else Path(a.state) / "oc.json"
try:
    c = json.loads(oc.read_text())
except (OSError, ValueError):
    c = {}
    print(f"warning: no overclock file ({oc}), the board stays on stock clock", file=sys.stderr)
raw = {}
try:
    s = serial.Serial(port, a.baud, timeout=0.05)
    s.reset_input_buffer()
    s.write(b"P 8 48\n"); time.sleep(0.3)   # stock 240 MHz while the bus is read (plain switch: no zones, no voltage)
    s.reset_input_buffer()
    s.write(b"F\n")
    buf, end, total = b"", time.time() + 20, None
    while time.time() < end and total is None:
        buf += s.read(8192)
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            l = l.decode(errors="replace").strip()
            if l.startswith("f end"):
                total = int(l.split()[2]); break
            if l.startswith("f "):
                try:
                    _, off, hx = l.split()
                    raw[int(off)] = bytes.fromhex(hx)
                except ValueError:
                    pass   # a garbled line: that strip stays black
    if c.get("div", 8) != 8:
        s.write(b"P %d %d %d %d\n" % (c["div"], c.get("mode", 0), c.get("ref", 0), c.get("vdig", 0))); time.sleep(0.3)   # with the voltage 320 MHz needs
    s.close()
finally:
    pause.unlink(missing_ok=True)

if not total:
    sys.exit("no screenshot from the board (is it running wired firmware v19 or later?)")
frame = bytearray(total)
got = 0
for off, b in raw.items():
    frame[off:off + len(b)] = b; got += len(b)
if got < total:
    print(f"warning: {total - got} of {total} bytes missing (black strips)", file=sys.stderr)

rows = []
for y in range(H):
    row = bytearray()
    for x in range(W):
        i = (y * W + x) * 3
        r, g, b = frame[i], frame[i + 1], frame[i + 2]
        if a.bgr: r, b = b, r
        px = bytes((r | r >> 6, g | g >> 6, b | b >> 6))   # 6-bit values in the top bits -> full range
        row += px * a.scale
    rows += [bytes(row)] * a.scale

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * a.scale, H * a.scale, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(b"\x00" + r for r in rows), 9)) + chunk(b"IEND", b"")
open(a.out, "wb").write(png)
print(f"{a.out}: {W * a.scale}x{H * a.scale}, {got}/{total} bytes")
