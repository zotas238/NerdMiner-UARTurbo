# NerdMiner UARTurbo: a 320 MHz ESP32-S3 NerdMiner

A fork of [NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2) (v1.8.3) for the ESP32-S3 that

1. makes the miner itself about 2x faster at stock clock (**Turbo**: hand-tuned SHA-256 engine driver + assembly software miner), and
2. turns Wi-Fi off, takes work over a **UART wire** from a small Python relay on any computer, and **overclocks the CPU from 240 to 320 MHz** (**Wired**).

| Build | Clock | Hashrate (LilyGo T-Display S3) |
|---|---|---|
| Stock NerdMiner v1.8.3, Wi-Fi | 240 MHz | 205-232 kH/s |
| UARTurbo, Wi-Fi mode | 240 MHz | ~480 kH/s |
| UARTurbo, wired mode | 240 MHz | ~553 kH/s |
| Wired, overclocked | 300 MHz | ~692 kH/s |
| Wired, overclocked, core voltage 30 | **320 MHz** | **~737 kH/s** |

320 MHz runs at core voltage 30 (1.25 V, ESP-IDF's own 240 MHz value), ~37 °C, with 0 bad shares and no reboots in a 60-minute soak. The CPU drops to a lower divider for the moments it formats numbers, parses or does double math ("slow zones", below). This is solo "lottery" mining: it's for fun, not profit.

> **Warning:** the overclock reprograms undocumented ESP32-S3 PLL registers. It worked on one board; yours may differ (silicon lottery). Several safety nets are built in (below), but you do this at your own risk.

---

## One firmware, two modes

The board picks its mode at every boot, by itself:

- **Wired mode** when a host is wired to the UART: a host's TX line idles high, so with the chip's own pull-down on, GPIO44 (board RX) reads high only when something drives it. Wi-Fi stays off, work comes from the relay, and the relay may overclock it.
- **Wi-Fi mode** when GPIO44 is left unconnected: normal NerdMiner Wi-Fi setup and stratum at the stock 240 MHz, with OTA updates and a JSON status on port 80.

If a board booted in Wi-Fi mode later hears work from the relay on the wire, it reboots into wired mode. A `D` on the wire in the first 300 ms after boot reboots into the ROM download mode in either mode.

The screen shows the hashrate, the best share ever (kept in flash across reboots and updates), the best share since this boot and the chip temperature, plus the Wi-Fi signal (dBm) in Wi-Fi mode.

## Why a wire? Why no Wi-Fi?

On the ESP32-S3 one PLL (the BBPLL, 480 MHz) clocks the CPU **and** Wi-Fi, USB, flash/PSRAM and the APB bus. Overclock it and Wi-Fi and the USB port stop working, because their clocks drift out of spec. UART0 can be clocked straight from the 40 MHz crystal instead, so a plain serial link on **GPIO43/GPIO44** keeps working at any CPU clock. The board does nothing but hash; a relay program on a computer talks stratum to the pool and passes jobs and shares over the wire at 921600 baud.

```
  pool  <--stratum/TCP-->  relay (any computer)  <--UART 921600, 3.3 V-->  ESP32-S3 (Wi-Fi off, 320 MHz)
```

The relay also re-checks every share the board finds before submitting it, so a hashing error caused by the overclock can never reach the pool. It shows up as `bad_cand` in the status instead.

> **Quickest route: [Universal Linux Node](docs/universal-linux-node.md).** Any Linux computer + a US$3 USB-to-UART adapter on a USB-A port + three wires to the pads **G / 43 / 44**, then `sudo wired/linux/install.sh --pool ... --user ...`. That guide covers which pads, the cable colours, a loopback and wire test (`wired/tools/wirecheck.py`), the service and the `nerd-wired` command.

---

## What you need

### The board

**Tested:** LilyGo **T-Display S3** (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM). The `NerdminerV2-Wired` env builds for exactly this board (`NerdminerV2-Turbo` builds the same firmware; it only differs in uploading over Wi-Fi OTA).

**Other ESP32-S3 boards:** probably workable, but untested. Everything that makes this fast (SHA engine driver, PLL overclock, UART0 on the crystal) is ESP32-S3 silicon, not board-specific. To port it:
- create a PlatformIO env for your board (start from its upstream env), add `board_build.partitions = partitions_turbo.csv`, `build_unflags = -Os`, and `-O2 -D NM_TURBO=1 -D NM_WIRED=1` to its build flags;
- it needs **≥ 8 MB flash** (two 3 MB app slots, the second at 0x400000) and **GPIO43/44 broken out** (they are U0TXD/U0RXD on every S3 module, usually labelled TX/RX, but check that your board doesn't route them to a USB-UART bridge chip);
- set the PSRAM/flash type (`board_build.arduino.memory_type`) and display driver to your board's. The T-Display S3 env uses `qio_opi`.

**Not supported:** classic ESP32, ESP32-S2, C3, C6, H2. The PLL registers, SHA engine and assembly are S3-specific. (The Turbo idea could be ported; the code can't just be recompiled.)

### The host: any computer with a 3.3 V UART

No Orange Pi needed. The relay is a single Python file (Python 3.8+, `pyserial`) and runs on Linux, macOS or Windows. It needs a serial port that can do **921600 baud at 3.3 V logic**:

| Host | Serial port | Notes |
|---|---|---|
| Raspberry Pi (any with a 40-pin header) | `/dev/ttyAMA0` (or `/dev/serial0`) | Pin 8 = TX, pin 10 = RX, pin 6 = GND. Use the full PL011 UART, not the mini-UART: in `config.txt` set `enable_uart=1` and `dtoverlay=disable-bt` (Pi 3/4/Zero W), and turn off the serial login console in `raspi-config` → Interface → Serial (login shell: **No**, hardware: **Yes**). Pi 5: `dtparam=uart0=on`, port `/dev/ttyAMA0`. |
| Orange Pi 5 | `/dev/ttyS3` | Enable overlay `uart3-m0` (`overlays=uart3-m0` in `/boot/armbianEnv.txt` or via `orangepi-config`). Pin 19 = TX, pin 21 = RX, pin 25 = GND. Not the 3-pin debug header: that's the system console. |
| Other SBCs (Rock, Banana, Libre, Odroid...) | `/dev/ttyS*` | Enable a header UART in its device-tree overlays; make sure no console/getty is on it. |
| Normal PC / laptop / Mac / NAS | `/dev/ttyUSB0`, `/dev/tty.usbserial-*`, `COM5` | A **USB-to-TTL serial adapter set to 3.3 V**: CP2102, CH340/CH343 or FT232RL all do 921600. Avoid cheap PL2303 clones. Never use a 5 V or RS-232 adapter: 5 V on GPIO44 can kill the S3. `--port auto` (the default) finds it. Full walkthrough: [Universal Linux Node](docs/universal-linux-node.md). |

**The board's own USB-C port can't be used for the link:** USB is clocked by the PLL and dies when you overclock. Use the USB port only to power the board and for the first flash.

### Wiring

| ESP32-S3 (T-Display S3 header) | | Host |
|---|---|---|
| **GPIO43** (U0TXD, board TX) | → | **RX** |
| **GPIO44** (U0RXD, board RX) | ← | **TX** |
| **GND** | — | **GND** |

TX goes to RX both ways, and the ground must be shared. On the T-Display S3 they are the header holes labelled **G**, **43** and **44**. Soldered pins or a short Dupont cable both work. Keep it under ~30 cm: 921600 baud doesn't like long wires. A loose contact here was the author's only real hardware problem (one direction worked, the other was silent), so if the relay sends work but never hears back, re-seat the wire to host RX first.

**Power:** a plain USB charger into the board's USB-C. Not a PC USB port that also talks to it; see *Troubleshooting*.

### A pool

Any stratum v1 pool: a public solo pool like `public-pool.io:21496` or `solo.ckpool.org:3333`, or your own (e.g. [public-pool](https://github.com/benjamin-wilson/public-pool) / ckpool on your LAN). The relay asks for share difficulty 0.00015 (`mining.suggest_difficulty`) so you see shares often. Pools that ignore that still work; you just see fewer shares.

---

## Setup

### 1. Get the firmware

**Prebuilt:** download `NerdminerV2-Wired_factory.bin` and `NerdminerV2-Wired_firmware.bin` from the [Releases](../../releases) page.

**Or build it** ([PlatformIO](https://platformio.org/install/cli) Core or the VS Code extension):

```bash
git clone https://github.com/zotas238/NerdMiner-UARTurbo && cd NerdMiner-UARTurbo
pio run -e NerdminerV2-Wired
# -> firmware/<version>/NerdminerV2-Wired_factory.bin   (full image, flash at 0x0)
#    firmware/<version>/NerdminerV2-Wired_firmware.bin  (app only, for wireota.py)
#    .pio/build/NerdminerV2-Wired/firmware.elf          (keep it: needed to decode crashes)
```

(The build reads an env var `NM_OTA_PASS` for the OTA password used in Wi-Fi mode. Unset means OTA without a password.)

### 2. First flash, over USB (one time only)

```bash
pip install esptool
# Hold BOOT, plug the board's USB-C into the computer, release BOOT (download mode).
esptool --chip esp32s3 erase-flash
esptool --chip esp32s3 -b 921600 write-flash 0x0 NerdminerV2-Wired_factory.bin
```

(Older esptool: `esptool.py ... erase_flash` / `write_flash`. Or the [ESP Web Flasher](https://espressif.github.io/esptool-js/) in Chrome: factory bin at offset 0x0.) Erasing first matters: it clears the OTA selection a previous firmware may have left.

Now unplug it from the computer, wire it up (above), and power it from a USB charger. There is no Wi-Fi setup: the wired firmware never turns Wi-Fi on, and the pool and BTC address live in the relay.

### 3. Run the relay on the host

```bash
pip install pyserial          # or: sudo apt install python3-serial
# --port: a header UART as below; for a USB-UART adapter leave it out (default auto)
python3 wired/relay/wired_relay.py \
    --port /dev/ttyAMA0 \
    --pool public-pool.io:21496 \
    --user bc1qYOURADDRESS.wired
```

Within a few seconds you should see `serial open`, `pool connected`, `authorized True`, and when the board boots an `H nerdminer-wired ...` hello. If the board was already running, power-cycle it or `echo R >> ~/.local/state/nerd-wired/cmd` to see the hello. Status is written every 2 s:

```bash
python3 -m json.tool ~/.local/state/nerd-wired/status.json | head -20
#  "accepted": 452, "rejected": 1, "cand": 453, "bad_cand": 0, "khs": 737.4, "board": {... "mhz": 320, "vdig": 30, "zone": [1, 449268, 1] ...}
```

`khs` = board hashrate, `accepted` = shares the pool took, `bad_cand` = shares whose hash was wrong (**must stay 0**; if not, lower the overclock), `board.mhz` = current CPU clock.

Run it at boot with systemd: see [`wired/systemd/nerd-wired.service`](wired/systemd/nerd-wired.service). The user needs permission to open the port (`sudo usermod -aG dialout $USER` on Debian/Pi OS, `uucp` on Arch, then log in again). On Windows use Task Scheduler or NSSM; on macOS, launchd.

At this point it mines at stock 240 MHz, about 553 kH/s.

### 4. Overclock

The relay keeps the overclock in `~/.local/state/nerd-wired/oc.json` and applies it 30 s after every boot of the board:

```json
{"div": 12, "mode": 112, "vdig": 30}
```

(`{"div": 11, "mode": 48}` = 300 MHz at stock voltage.) Then restart the relay, or reboot the board (`echo R >> ~/.local/state/nerd-wired/cmd`). CPU MHz = 40 × (div + 4) / 2:

| div | CPU | mode | Author's result |
|---|---|---|---|
| 8 | 240 MHz (stock) | 0 | ~553 kH/s |
| 9 | 260 MHz | 48 | ~599 kH/s |
| 10 | 280 MHz | 48 | ~645 kH/s |
| 11 | 300 MHz | 48 | ~692 kH/s, stock voltage, about one crash a day |
| **12** | **320 MHz** | **112** + `"vdig": 30` | **~737 kH/s, core voltage 30, ~37 °C; 60-min soak: 0 bad, 0 reboots** (see "How 320 MHz works") |
| 13 | 340 MHz | 112 + `"vdig": 30` | the PLL locks (forced band + VCO current 0) but the CPU crashes within seconds. Don't. |
| 14 | 360 MHz | | the PLL never locks |

Go up one step at a time and watch `bad_cand`, `rejected` and the reboot count (`boots`) for 15 minutes at each step. The relay never steps the clock down by itself: if your chip keeps rebooting at a step, go back one.

**Trying 320 MHz on your chip:** run 300 first. Then set `{"div": 12, "mode": 112, "vdig": 29}` and watch for an hour; if it reboots, try `"vdig": 30`; if it still does, your chip's limit is 300. The voltage code is relative to your chip: check the stock value with `V` first (the author's is 27), and don't go above 30. To find your margin before mining at it, starting from 300 MHz, park the PLL at 640 with the CPU at PLL/3 (`I 0 0`, `I 600C0010 3 1`, `K`, `K 1 4 4 1`, `K 1 3 0 0`, `P 12 48`, one per line), set a voltage (`V 29`), and run the stress kernels with both cores busy (`N 4 300 1`, `N 7 300 1`; see the command reference): they failed within 30 runs at the author's stock voltage and passed thousands at 29. Reboot (`R`) when done.

**Mode bits** (add them up):

| bit | meaning |
|---|---|
| 4 | keep flash/PSRAM in high-speed mode. Leave it off: above 280 MHz flash/PSRAM panic, and low-speed mode costs no hashrate |
| 16 | VCO bias 3 for overclocked dividers (needed for a clean lock) |
| 32 | gentle switch onto the new PLL frequency (CPU at PLL/6 first, then PLL/2, 5 ms settle). Without it about one switch in three glitches |
| 64 | **zoned** (for 320 MHz): raises the core voltage to `vdig` first, lands the switch with the CPU at PLL/3, forces the VCO band above 600 MHz (plus VCO current 0 at div 13), then turns on slow zones |
| 8 | restart VCO calibration (experiment) |
| 1, 2 | **don't use**: 1 leaves the CPU stuck on the crystal, 2 hangs |

**Safety nets** (built in, nothing to set up):
- **RTC watchdog** (8 s, runs off the RTC slow clock, not the PLL): a hard hang causes a full chip reset, which also puts the PLL and voltage back to stock.
- **No slow modes when wired:** there are two modes only, wired turbo (~735 kH/s at 320 MHz) and Wi-Fi (stock clock, lower hashrate). Wired, the board always runs the fastest engine (t5): after a crash it reboots straight back into it at the `oc.json` clock (the relay never steps the clock down), and a wrong engine hash costs only that nonce (counted in `bad`). If the t5 self-test fails at boot, the board retries it and then reboots.
- **Relay takes the port back:** whenever the relay reopens the serial port (after `wireota.py`, `screenshot.py` or esptool) it re-applies the overclock 30 s later, so a reboot it didn't see can't leave the board on stock clock.
- **Self-tests:** every fast SHA path is checked against mbedTLS at boot and spot-checked while mining (`bad` / `sw_bad` in status must stay 0). In Wi-Fi mode a failing path falls back to a slower one, and two crash resets in a row put the engine in a slow safe mode (`"safe": true`, cleared by `Z`).
- **The relay re-hashes every share**, so a wrong result never reaches the pool.

Core voltage: `echo V >> .../cmd` shows it (e.g. eFuse value 27 ≈ 1.19 V); `V 28` raises it, capped at 30 (1.25 V, ESP-IDF's 240 MHz value). Every boot puts it back to the chip's own value. 320 MHz uses 30; 300 MHz and below run at stock.

#### How 320 MHz works

Measured with the `N` stress kernels (below) on the author's chip:

- The VCO's calibration tops out between 600 and 640 MHz, so 640 (div 12) locks only with the band forced to its fastest setting. Once locked it runs fine: with the CPU at PLL/3 (213 MHz) the board mines and every result checks out.
- **One core alone passes everything at 320 MHz** (SHA engine, multiplier, SRAM, flash code, double math, `snprintf`), even at stock voltage. It fails only when **both cores hit internal SRAM at the same time**: that fails within milliseconds at voltage 27 and passes thousands of runs at 29. Switching the CPU from the crystal straight to PLL/2 on a freshly forced PLL crashes at once, so mode 64 lands at PLL/3 first.
- The weakest code at high clocks is `snprintf("%g")` → `_dtoa_r` → ROM soft-float: the daily 300 MHz crash and the 320 crashes without zones were all there. **Slow zones** drop the CPU to PLL/3 (one register write, ~1 µs with the other core stalled) while anything formats, parses or does double math (the wire task, candidate checks, the screen) and put it back to PLL/2 after. The SHA loops run at full clock the rest of the time. Cost: about 1%. Status `"zone"`: [on, switches, depth].
- PSRAM and flash are clocked from the PLL through the MSPI bus, not from the CPU divider, so zones can't protect them. Wired mode doesn't use PSRAM: at boot all heap allocations are sent to internal RAM and the free PSRAM is held (status `"psram"`: [total, free before, held, free now]). 340 MHz (div 13) locks, but crashes in the CPU itself within seconds, even with the fence and voltage 30.

### 5. Update the firmware over the wire

No buttons, no USB, works while overclocked:

```bash
python3 wired/tools/wireota.py --port /dev/ttyAMA0 NerdminerV2-Wired_firmware.bin
```

It pauses the relay, streams the app image (~2-3 min), the board validates it and boots it, and the relay resumes. A broken transfer leaves the old firmware in place. Each chunk carries a CRC32 and the board refuses a damaged one, so bytes lost on the wire only cost a resend; if the transfer gets out of step, the script starts it over (up to 3 times). Use the `_firmware.bin`, not the `_factory.bin`.

---

## Relay command reference

Write one per line to `<state>/cmd` (`echo S >> ~/.local/state/nerd-wired/cmd`). Replies land in `relay.log` and `status.json` → `pll_replies`.

| cmd | does |
|---|---|
| `S` | status now (`T {json}`) |
| `R` | reboot the board |
| `P <div> <mode> [ref] [vdig]` | switch the PLL now (`P 8 0` = back to stock, `P 12 112 0 30` = 320 MHz zoned). Not saved; edit `oc.json` for that |
| `Q <div> <mode>` | dry run: lock the PLL at div, switch straight back |
| `V [n]` | read / set core voltage code (capped at 30) |
| `Z` | clear the crash guard (Wi-Fi mode's safe mode) and reboot |
| `W <s>` | screen redraw period in seconds (default 12, `0` = frozen). Status `"redraw"`: [period, last µs, max µs, count] |
| `L <h> <h>` | block height for the screen (the relay sends it from the job's coinbase, BIP34, when it changes) |
| `M <d> <d>` | set the best-ever share difficulty shown on the screen (value twice, both must match) |
| `F` | screenshot: use `wired/tools/screenshot.py out.png` (drops to stock clock for the read-back, restores the overclock) |
| `Y` | stream the last coredump (use `wired/tools/coredump_get.py <firmware.elf>`) |
| `X` | dump BBPLL registers (now and at boot) |
| `K <reg> <msb> <lsb> <val>` / `K` | extra BBPLL register writes for the next switch / clear them (experiments) |
| `B` | PIE SIMD micro-benchmark |
| `I <addr> <mask> <val>` | hex read-modify-write of one SYSTEM clock register (`0x600C0000` block only; mask 0 = read) and the true CPU MHz against the crystal timer. `I 0 1` / `I 0 0` turns slow zones on / off |
| `N <kernel> <reps> [1]` | stress test: with the CPU parked at PLL/3, run a kernel at PLL/2 and compare it with the PLL/3 result. Kernels: 0 idle, 1 SHA engine loop, 2 multiply, 3 ALU, 4 SRAM, 5 code from flash, 6 double math, 7 `snprintf %g`. Flag 1 leaves the other core mining at full clock (both cores busy); otherwise it is stalled |
| `U` | SHA register bus benchmark: access costs and the t5 loop with the split-burst setting on and off, checked against mbedTLS (holds the engine < 1 s) |
| `D` | reboot into ROM download mode. **Don't:** the ROM doesn't answer on the wire afterwards, and you'll need the USB recovery below |

Wire protocol (board side, `runWiredWorker` in `src/mining.cpp`): relay → board `J <id> <pool diff> <80-byte header hex>` (new work), `A <diff>` (share accepted); board → relay `H ...` (hello at boot, with reset reason and last PLL setting), `T {json}` (status every 2 s), `N <id> <nonce> <diff>` (share candidate).

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `serial open` but no `H` hello, `khs` null | TX/RX swapped, missing GND, or a bad joint on GPIO43 → host RX. Is the console/getty still on that UART? Did the first flash succeed? |
| Hello OK, but no shares | Check `authorized True` (address/pool), and that the pool's difficulty isn't huge (`pool diff` lines in the log). |
| `bad_cand` > 0, `rejected` climbing, frequent `boots` | Overclock too high for your chip: lower `div` (or at 320 MHz raise `vdig` to 30), or add/remove mode bit 4 per the table. |
| ~715 kH/s at 320 MHz instead of ~737, `t5.sb` code 5 | The board crashed once and runs this boot without the split-burst speed-up (the crash guard). If it was a one-off, `R` reboots it back to full speed. |
| A few `rejected` with `Job not found` | The share was for a job the pool had already dropped (stale). Normal, not the overclock: the relay re-hashed it. |
| Hashrate ~275 kH/s, `"safe": true` (Wi-Fi mode only) | Crash guard tripped: fix the cause, then `Z`. |
| Board dead / boot loop / anything else | **USB recovery:** hold BOOT, plug USB into a computer, and do the first-flash steps again. The ROM bootloader lives in mask ROM, so it can always be reached this way. |
| Hashrate drops when the USB-C is plugged into a PC | Power the board from a charger: a USB host that opens the board's CDC port can slow it down. |

---

## How the speed-up works (short version)

- **HW miner** (≈ 92 % of the hashrate): drives the S3's SHA accelerator with raw register stores, batches 128 nonces (first blocks for all, then the second hashes), replaces BUSY polling with calibrated cycle-count waits, and moves digests with PIE 128-bit loads/stores: ~455 CPU cycles per nonce, against ~1200 in stock NerdMiner. Most of those cycles are the CPU talking to the SHA registers over the peripheral bus (~16 cycles per word). Two wired-mode tricks cut that:
  - **Split burst off:** the S3 splits 128-bit accesses to peripherals into single transfers by default (`SENSITIVE_APB_PERIPHERAL_ACCESS_SPLIT_BURST`). Clearing it around the mining loop saves ~13%. It is used only if 4096 nonces match mbedTLS with it and it benches faster at boot; status `t5.sb` = [cycles ×10 with it, without it, code: 1 used, 2 last trial hung, 3 self-test failed, 4 not faster, 5 off after a crash].
  - **No anchor on the second hash:** the second hash of each nonce after the first in a batch starts without an anchor load, with interrupts masked around the START store and the wait timed from its issue (~1%). The H7 read is occasionally early (~0.7% of those nonces): such a nonce is lost, and a false candidate from it is counted in `t5.nb[9]` against `t5.nb[10]` (those nonces / 65536), not as an engine fault. It switches itself off if the loss passes 1%.

  Every variant is self-tested against mbedTLS at boot and the fastest passing one is picked; every 8th job is spot-checked against mbedTLS while mining.
- **SW miner** on the other core: double SHA-256 in generated, list-scheduled Xtensa assembly with schedule/round precompute: ~5450 cycles per nonce.
- **Wired:** stratum moves off the chip, the UART goes onto the crystal, and the BBPLL feedback divider is reprogrammed at run time following ESP-IDF's own PLL sequence (MSPI to low speed, CPU on the crystal, recalibrate, wait for lock, gentle return). That's in `src/turbo/turbo_pll.cpp`. The hashrate scales with the **PLL**, not just the CPU divider: the peripheral bus is clocked from the PLL independently of the CPU divider, and most of the miner's cycles are bus traffic. At 320 MHz, slow zones keep the code that formats, parses and does double math at PLL/3 while the SHA loops run at PLL/2, and wired mode keeps the heap out of PSRAM (see "How 320 MHz works").

Main files: `src/turbo/turbo_hw.cpp` (HW engine), `src/turbo/sha256_sw*.{cpp,S,inc}` + `gen_sw_asm.py` (SW miner), `src/turbo/turbo_pll.cpp` (overclock), `src/mining.cpp` → `runWiredWorker` (wire protocol), `wired/` (relay and tools).

In Wi-Fi mode (no wire, ~480 kH/s at stock clock) the firmware adds ArduinoOTA and a JSON status on port 80; update it there with `pio run -e NerdminerV2-Turbo -t upload`.

## Credits & license

Based on [NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2) by BitMaker and contributors (MIT); its original README is in [README-upstream.md](README-upstream.md). Turbo and Wired changes are released under the same [MIT license](LICENSE).
