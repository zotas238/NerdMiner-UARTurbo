#!/usr/bin/env python3
"""Update the RUNNING wired NerdMiner over the UART wire (its own OTA: no ROM, no buttons, ~2-3 min).

usage: wireota.py [--port auto|/dev/ttyUSB0|/dev/ttyAMA0] [--state DIR] <NerdminerV2-Wired_firmware.bin>

Pauses the relay (creates <state>/pause), sends O/C/E (see runWiredWorker in src/mining.cpp),
then resumes the relay. The board writes its spare OTA slot, esp_ota_end validates the image,
and only then does it boot it, so a cut-off transfer leaves the old firmware running.
Use the *_firmware.bin (app image), not the *_factory.bin.
"""
import argparse, hashlib, os, sys, time, zlib
from pathlib import Path
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "relay"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nerdport

ap = argparse.ArgumentParser()
ap.add_argument("--port", default=os.environ.get("NERD_PORT", "auto"), help="default auto (see nerdport.py)")
ap.add_argument("--baud", type=int, default=int(os.environ.get("NERD_BAUD", "921600")))
ap.add_argument("--state", default=os.environ.get("NERD_WIRED_STATE", str(Path.home() / ".local/state/nerd-wired")))
ap.add_argument("--pause", help="pause file the relay watches (default <state>/pause)")
ap.add_argument("firmware")
a = ap.parse_args()
try:
    port = nerdport.resolve(a.port)
except LookupError as e:
    sys.exit(str(e))

fw = open(a.firmware, "rb").read()
if fw[:1] != b"\xe9":
    sys.exit("not an ESP app image (use NerdminerV2-Wired_firmware.bin, not the factory image)")
sha = hashlib.sha256(fw).hexdigest()
pause = Path(a.pause) if a.pause else Path(a.state) / "pause"
pause.parent.mkdir(parents=True, exist_ok=True)
pause.touch()
time.sleep(1.5)   # let the relay let go of the port
try:
    s = serial.Serial(port, a.baud, timeout=0.05)
except Exception:
    pause.unlink(missing_ok=True)   # don't leave the relay paused
    raise
s.reset_input_buffer()
buf = b""


def reply(prefixes, t=5.0):
    global buf
    end = time.time() + t
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            l = l.decode(errors="replace").strip()
            if l.startswith(prefixes):
                return l
    return None


CH = 512


def board_offset(r):
    """The board's write offset from a 'c N' / 'c! off N [N]' reply, or None if garbled.
    Firmware with CRC support repeats N, and both copies must agree."""
    w = r.split() if r else []
    try:
        if w[0] == "c" and len(w) == 2:
            return int(w[1])
        if w[0] == "c!" and len(w) == 3:
            return int(w[2])
        if w[0] == "c!" and len(w) == 4 and w[2] == w[3]:
            return int(w[3])
    except (IndexError, ValueError):
        pass
    return None


def transfer():
    """One full pass: O, chunks, E. True when the board validated and accepted the image.
    Bytes do get lost on the wire now and then. With CRC firmware the board refuses a bad
    chunk and it's resent. Older firmware can take a chunk short a hex digit (it then sits
    off a chunk boundary with bad data written); that pass is abandoned and started over."""
    s.reset_input_buffer()
    s.write(f"O {len(fw)} {sha}\n".encode())
    r = reply(("o ",), 15)
    print("start:", r, flush=True)
    if not r or not r.startswith("o 1"):
        sys.exit("board did not start the update (is it running the wired firmware and wired right?)")
    crc = r.split()[-1] == "crc"
    off, t0, stall, odd = 0, time.time(), 0, None
    while off < len(fw):
        chunk = fw[off:off + CH]
        tail = f" {zlib.crc32(chunk):08x}" if crc else ""
        s.write(f"C {off} {chunk.hex()}{tail}\n".encode())
        r = reply(("c ", "c!"), 3)
        if r == f"c {off + len(chunk)}":
            off += len(chunk); stall = 0
            if off % (CH * 256) == 0:
                print(f"  {off}/{len(fw)} {off / (time.time() - t0) / 1024:.1f} KB/s", flush=True)
            continue
        stall += 1
        b = board_offset(r)
        print("  retry at", off, r, flush=True)
        if crc and off == 0 and stall >= 6:
            print("  board refuses every CRC chunk: sending plain chunks", flush=True); crc = False
        if stall > 40:
            print("  no progress, starting over", flush=True); return False
        if b is None or b > len(fw):
            continue                       # garbled reply: send the chunk again
        if b % CH and b != len(fw):
            # the board took a chunk short of a digit: its image is bad from there on. Seen twice
            # (not a garbled reply), start the whole transfer again.
            if odd == b:
                print(f"  board at {b}, off a chunk boundary: starting over", flush=True); return False
            odd = b
            continue
        off = b                            # go to where the board is (a lost ack, or a resend)
    print(f"sent {len(fw)} bytes in {time.time() - t0:.0f} s", flush=True)
    s.write(b"E\n")
    r = reply(("e ",), 30)
    print("end:", r, flush=True)
    return bool(r) and r.startswith("e 1")


try:
    for attempt in range(3):
        if transfer():
            break
    else:
        sys.exit("update failed 3 times; the old firmware is still running")
finally:
    s.close()
    pause.unlink(missing_ok=True)
