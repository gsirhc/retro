# Altair 8800

Intel 8080 CPU core in C++ with an Altair 88-2SIO serial board and an 88-DCDD
floppy controller (enough to boot CP/M), a native demo, a GoogleTest suite, and
a WebAssembly + xterm.js browser front end.

## Commands

Run from this directory — no arguments, `package.json`-style:

| Command | What it does |
|---|---|
| `make demo`  or `scripts/demo`  | build + run the native serial-echo demo |
| `make test`  or `scripts/test`  | build + run the GoogleTest suite (88-DCDD + 88-ACR) |
| `make cpmdisk DISK=x.dsk`        | boot a CP/M diskette image on the emulated 88-DCDD |
| `make serve` or `scripts/serve` | build the wasm module + serve the browser UI on `:8000` |
| `make wasm`   | build `web/retro8080.{js,wasm}` only |
| `make check`  | `demo` + `test` |
| `make clean`  | remove all build output |
| `make`        | list the above |

## Layout

| Path | |
|---|---|
| `../shared/cpu/cpu_i8080.{h,cpp}` | the CPU core, shared with Space Invaders, tested there |
| `serial2sio.{h,cpp}` / `ringbuffer.h` | 88-2SIO board, ring-buffered RX/TX on ports `0x10`–`0x13` |
| `cassette.{h,cpp}` | MITS 88-ACR audio-cassette interface, ports `0x06`/`0x07` (BASIC `CSAVE`/`CLOAD`) |
| `disk88.{h,cpp}` | MITS 88-DCDD 8-inch floppy controller, ports `0x08`–`0x0A` |
| `disk_bootrom.h` | the 256-byte MITS disk bootstrap PROM (entry `0xFF00`) |
| `main.cpp` | native harness: 64K RAM + board, runs an echo program |
| `cpm/cpm_disk.cpp` | native CP/M boot test — mounts a `.dsk`, runs the turnkey boot |
| `tests/` | GoogleTest suite (CMake + FetchContent) |
| `web/` | Emscripten wrapper, xterm.js front end (`vendor/` holds xterm) |

`disk88.{h,cpp}` is written from MITS's own 88-DCDD documentation (the
"Altair Disk Controller I/O Information" pages and the controller schematics)
and Pertec's FD-400 operating and service manual: 360 rpm rotation, a byte
every 32 µs, the head-load, step and settle timing, and a track-0 line that
follows the head. `disk_bootrom.h` is the DBL PROM image; its RAM-resident code
matches a dumped DBL 3.3 PROM (deramp.com `DBL33.PRN`). See `ALTAIR_REVIEW.md`
§3.2 for the sources and what is still unsourced.

## Toolchain

- `make demo` / `make test` — a C++17 compiler; `make test` also needs CMake
  (GoogleTest is fetched on first configure).
- `make serve` / `make wasm` — the [Emscripten SDK](https://emscripten.org)
  on `PATH` (`brew install emscripten`, or `source emsdk_env.sh`).
