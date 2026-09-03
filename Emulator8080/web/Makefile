# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

# Use the C++ driver (em++), not emcc — embind pulls in libc++abi typeinfo
# that the C driver won't link.
EMCC ?= em++

SRC = wasm_machine.cpp ../i8080.cpp ../serial2sio.cpp ../disk88.cpp ../cassette.cpp
HDR = ../i8080.h ../serial2sio.h ../ringbuffer.h ../disk88.h ../disk_bootrom.h ../cassette.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=Retro8080 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

retro8080.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

.PHONY: roms media serve test test-install clean
roms: roms/echo.bin
roms/echo.bin roms/hello.bin roms/killbits.bin: roms/gen_roms.py
	python3 roms/gen_roms.py

# Altair BASIC + CP/M 2.2, pinned + checksummed (git-ignored)
media:
	bash roms/fetch-basic.sh
	bash disks/fetch-cpm.sh

serve: retro8080.js roms media
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 -m http.server 8000

# Playwright integration + fidelity suite (tests/). Needs Node; run
# `make test-install` once to pull the runner and a headless Chromium.
test: retro8080.js roms media
	npx playwright test

# same suite, one worker, with V8 line/branch/function coverage for app.js
# -> coverage/index.html  (+ coverage/lcov.info)
coverage: retro8080.js roms media
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f retro8080.js retro8080.wasm roms/echo.bin roms/hello.bin roms/killbits.bin
	rm -rf test-results playwright-report coverage .v8-coverage
