# Emulator8080

Intel 8080 CPU core in C++ with an Altair 88-2SIO serial board, a native demo,
a GoogleTest suite, and a WebAssembly + xterm.js browser front end.

## Commands

Run from this directory — no arguments, `package.json`-style:

| Command | What it does |
|---|---|
| `make demo`  or `scripts/demo`  | build + run the native serial-echo demo |
| `make test`  or `scripts/test`  | build + run the GoogleTest arithmetic suite |
| `make serve` or `scripts/serve` | build the wasm module + serve the browser UI on `:8000` |
| `make wasm`   | build `web/retro8080.{js,wasm}` only |
| `make check`  | `demo` + `test` |
| `make clean`  | remove all build output |
| `make`        | list the above |

## Layout

| Path | |
|---|---|
| `i8080.{h,cpp}` | the CPU core — registers, flags, `step()` decoder, `Bus` callbacks |
| `serial2sio.{h,cpp}` / `ringbuffer.h` | 88-2SIO board, ring-buffered RX/TX on ports `0x10`–`0x13` |
| `main.cpp` | native harness: 64K RAM + board, runs an echo program |
| `tests/` | GoogleTest suite (CMake + FetchContent) |
| `web/` | Emscripten wrapper, xterm.js front end (`vendor/` holds xterm) |

## Toolchain

- `make demo` / `make test` — a C++17 compiler; `make test` also needs CMake
  (GoogleTest is fetched on first configure).
- `make serve` / `make wasm` — the [Emscripten SDK](https://emscripten.org)
  on `PATH` (`brew install emscripten`, or `source emsdk_env.sh`).
