# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

# Use em++, not emcc: embind needs libc++abi typeinfo the C driver won't link.
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

.PHONY: shared roms media serve test test-smoke test-install clean
roms: roms/echo.bin
roms/echo.bin roms/hello.bin roms/killbits.bin: roms/gen_roms.py
	python3 roms/gen_roms.py

# Altair BASIC and diskette images, pinned and checksummed (git-ignored)
media:
	bash roms/fetch-basic.sh
	bash disks/fetch-disks.sh

# Shared chrome is copied from retroweb/shared/ into web/shared/ (git-ignored),
# since only web/ gets staged.
SHARED = ../../shared
shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js $(SHARED)/winbtns.js \
	   shared/

serve: retro8080.js roms media shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 -m http.server 8000

# Playwright suite; `make test-install` once for the runner and Chromium
test: retro8080.js roms media shared
	npx playwright test

# smoke.spec.ts only, what deploy CI runs
test-smoke: retro8080.js roms media shared
	npx playwright test tests/smoke.spec.ts

# one worker, V8 coverage for app.js -> coverage/index.html + lcov.info
coverage: retro8080.js roms media shared
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f retro8080.js retro8080.wasm roms/echo.bin roms/hello.bin roms/killbits.bin
	rm -rf shared
	rm -rf test-results playwright-report coverage .v8-coverage
