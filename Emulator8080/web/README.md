# retro 8080 — browser front end

WebAssembly build of the 8080 core + Altair 88-2SIO board, wired to an
[xterm.js](https://xtermjs.org/) terminal.

`vendor/` holds xterm 5.3.0 + the fit addon and two bitmap fonts (VT323,
Courier Prime — both OFL), checked in so the page needs no network at runtime.
Refresh xterm with:

```sh
curl -Lo vendor/xterm.js            https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.js
curl -Lo vendor/xterm.css           https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.css
curl -Lo vendor/xterm-addon-fit.js  https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.js
```

```
xterm keystrokes ─► Machine.sendByte()      → 2SIO channel A RX ring buffer
Machine.readOutput() ─► xterm.write()        ← 2SIO channel A TX ring buffer
```

## Build

Needs the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH` (`source ./emsdk_env.sh`, or `brew install emscripten`). The Makefile
links with `em++` — the C driver (`emcc`) won't pull in the libc++abi typeinfo
that embind needs.

```sh
make          # -> retro8080.js + retro8080.wasm
make serve    # build, then python3 -m http.server 8000
```

Open <http://localhost:8000>. (Serve over HTTP — browsers won't fetch `.wasm`
from `file://`.)

## Files

| File | Role |
|---|---|
| `wasm_machine.cpp` | embind wrapper: `Machine` = sized RAM + `i8080::Cpu` + `Serial2SIO` + `Disk88` + `CassetteACR` |
| `index.html` | page shell + the "Load Program" / "Insert Diskette" dialogs; loads xterm + the wasm module |
| `app.js` | terminal wiring, the per-frame `runCycles` loop, program loader, front panel, 88-DCDD cabinet, register readout |
| `roms/` | built-in programs + `manifest.json` catalog the loader fetches (see `roms/README.md`) |
| `disks/` | 8-inch floppy images + `manifest.json` for the 88-DCDD cabinet (see `disks/README.md`) |

The footer's "Last built" date comes from a `HEAD` request for
`retro8080.wasm` — its `Last-Modified` header, i.e. the last `make wasm`.

## Runtime model

`app.js` calls `machine.runCycles(2_000_000 / 60)` once per `requestAnimationFrame`,
then drains `machine.readOutput()` to the terminal. The 2SIO ring buffers
(512 bytes each direction) absorb the mismatch between the emulated 2 MHz clock
and the browser's frame cadence, so no bytes are lost between frames.

## Page theme

**Theme:** in the toolbar switches the page chrome (not the terminal):

- **Windows** (default) — teal desktop, navy title bar, beveled gray window.
- **1994 Web** — flat `#c0c0c0` Mosaic/Netscape-1.x gray, no window chrome,
  serif left-aligned headings, thin purple rules.
- **Modern** — clean white card, system font, no title bar; a plain tagline
  and spec line instead of the `~ * ~` one.

Themes are CSS custom properties keyed off `:root[data-theme]`; an inline
`<head>` script applies the stored choice before first paint (no flash).
`?theme=web94` overrides.

## Terminal profiles

The **Terminal:** dropdown swaps the xterm face, phosphor colour, CRT overlay
(scanlines / paper texture / glow / flicker), cursor, CAPS default, and **baud
rate**. Choices: Modern (instant), DEC VT100 green/amber (9600), DEC VT52
(4800, heavy scanlines), Lear Siegler ADM-3A (9600, underline cursor), Glass TTY
(300), Teletype ASR-33 (110, uppercase, cream paper, mechanical bell).

Baud is real: output is metered onto the screen at `baud/10` chars/s, and
because the 2SIO's 512-byte FIFO fills and drops TDRE, an output-bound program
(BASIC `LIST`) actually *waits* for the terminal — a 110-baud Teletype throttles
the 8080 exactly as it did in 1975. Choice persists in `localStorage`;
`?term=vt52` overrides.

## Front panel

`app.js` builds an Altair-8800 panel above the terminal, driven by the wasm
`Machine`:

- **Address LEDs** = PC, **Data LEDs** = the byte at PC, status LEDs
  (`WAIT`/`HLTA`/`INTE`/`MEMR`/`MI`) from `state()`, refreshed every frame.
- **A0–A15 toggles** — the top 8 (`A8`–`A15`) feed `setSenseSwitches()` live.
- Bat-handle paddles: click the **top half** for a switch's upper function,
  the **bottom half** for the lower one. **STOP/RUN** pauses the frame loop;
  **SINGLE STEP** runs one instruction (`stepOne`) while stopped;
  **EXAMINE / EXAMINE NEXT** set PC from the switches / PC+1;
  **DEPOSIT / DEPOSIT NEXT** poke `writeByte`; **RESET/CLR** = `reboot` + PC 0.
- **OFF/ON** kills the machine and blanks every LED.
- `PROTECT`, `AUX` paddles are cosmetic.

## Era presets

The **PRESET** dropdown at the top configures a period-correct machine in one
click — RAM size, primary terminal, which S-100 cards are plugged in (shown in
the backplane strip below the panel), the visible peripherals, and, with
**Auto-load software** ticked, the flagship program running:

| id | Preset | RAM | Terminal | Software |
|---|---|---|---|---|
| `baremetal` | Bare-Metal Toggle (1975) | 4 KB | ASR-33 | Kill the Bit (bundled) |
| `stock` | Stock Launch (1975) | 4 KB | ASR-33 | Altair 4K BASIC *(you supply)* |
| `cassette` | Cassette Hobbyist (1976) | 32 KB | ADM-3A + 88-ACR | 8K BASIC + Star Trek |
| `cpm` | CP/M Workstation (1978) | 64 KB | VT100 + 88-DCDD | CP/M 2.2 |

RAM is contiguous from 0 (`machine.setRam(kb)`); above it the bus floats high,
so BASIC's `MEMORY SIZE?` auto-detect lands on the real number and a 4 KB
machine can't run an 8 KB program. A ROM image loaded above the ceiling (the
0xE000 BASIC build, the disk PROM) stays readable.

Uncheck **Auto-load** to get just the hardware and load software yourself.
Changing RAM/terminal/disk by hand flips the dropdown to *— custom —*. The
choice persists; `?preset=cpm` in the URL wins over the stored one.

Presets 4 (Cromemco Dazzler) and 5 (Processor Tech VDM-1) are not built yet.

## Loading programs

**Load Program...** opens a dialog listing the built-ins from `roms/manifest.json`
(fetched from the server, each entry probed for availability) plus a
load-from-disk file picker. Each entry carries a load address and a start (PC);
those pre-fill the fields and are editable for disk loads. **RESET** re-applies
the last program loaded, or falls back to the built-in echo ROM.

**Load via: memory (instant) | paper tape.** Paper tape is the period-correct
path: a guide appears under the panel with a 25-byte serial bootstrap in octal,
you key it into the front panel (EXAMINE → DEPOSIT/DEPOSIT NEXT ×25 → EXAMINE →
RUN — or hit *Enter it for me*), then the image trickles through the 2SIO
receive line while the address LEDs climb through memory. Tuned to ~7 s (a real
Teletype reader took ~14 min for 8 KB). Needs the `make wasm` rebuild for
`writeByte`/`setPC`/`rxPending`. `?tapedemo` in the URL pre-opens the guide.

Altair 4K BASIC is listed but not bundled — see `roms/README.md`.

## Disks and CP/M

The **MITS 88-DCDD cabinet** below the front panel is two 8-inch floppy drives
on ports `0x08`–`0x0A`. Click a drive to open **Insert Diskette** — pick from
`disks/manifest.json` or choose a local `.dsk` — then press **BOOT** on the
cabinet (or do it from the panel: `EXAMINE` `0xFF00`, `RUN`). `machine.bootDisk()`
drops the 256-byte MITS bootstrap PROM at `0xFF00` and starts there; it reads
track 0 of drive A and hands off to the diskette's cold-start loader.

Standard MITS images are 337,568 bytes (77 × 32 × 137). They're **not bundled**
(`*.dsk` is git-ignored) — see `disks/README.md` for where to get CP/M 2.2,
WordStar, Zork, etc. CP/M needs the full 64 KB of RAM, which the machine has
(a period build got there with four 88-16MCD 16 KB dynamic-RAM boards).

A drive's red lamp flickers on head-load, seek, and data transfer. Writes
(`SAVE`, `ED`, `PIP`) go to the in-memory image; a **SAVE** button appears on a
drive with unsaved changes and downloads the modified `.dsk`. **RESET** re-runs
the disk boot with the diskettes still in their drives.

Each loaded drive has a **?** button — a "How to use" dialog with that disk's
programs (how to start and quit them) plus a CP/M command primer. The text
comes from the `help` / `os` fields in `disks/manifest.json`.

## Cassette (88-ACR)

The **88-ACR deck** (ports `0x06`/`0x07`) appears when a preset includes it.
Click it to insert a blank tape (or a `.cas` file you saved before), then in
BASIC: `CSAVE"X"` records the current program, `CLOAD"X"` plays it back.
**REWIND** between the two, or just re-insert the file. There's no audio model —
the "tape" is a byte buffer with a play/record head — but the status polarity
matches what our 8K BASIC ROM polls (`cassette.cpp` has the details). A tape
with an unsaved recording grows a **SAVE** button that downloads the `.cas`.
