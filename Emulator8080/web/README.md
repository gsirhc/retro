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
| `wasm_machine.cpp` | embind wrapper: `Machine` = 64K RAM + `i8080::Cpu` + `altair::Serial2SIO` |
| `index.html` | page shell + the "Load Program" dialog; loads xterm + the wasm module |
| `app.js` | terminal wiring, the per-frame `runCycles` loop, program loader, register readout |
| `roms/` | built-in programs + `manifest.json` catalog the loader fetches (see `roms/README.md`) |

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
