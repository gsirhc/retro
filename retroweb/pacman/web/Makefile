# Build the WebAssembly module. Needs the Emscripten SDK on PATH.

EMCC ?= em++
CORE = ..
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/cpu_z80.cpp $(CORE)/video.cpp $(CORE)/wsg.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CORE)/cpu_z80.h $(CORE)/video.h $(CORE)/wsg.h hwtest_roms.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=PacmanArcade \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sSTACK_SIZE=1MB

pacman.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) -I. -I$(CORE) $(SRC) -o $@

hwtest_roms.h: $(CORE)/roms/hwtest/gen_hwtest.py
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h

.PHONY: shared roms wasm serve test test-install clean
wasm: pacman.js
roms: hwtest_roms.h
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h \
	  --png ../../assets/pacman-cabinet.png

SHARED = ../../shared
shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js \
	   shared/

serve: pacman.js roms shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: pacman.js roms shared
	npx playwright test

coverage: pacman.js roms shared
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f pacman.js pacman.wasm hwtest_roms.h
	rm -rf roms shared
	rm -rf test-results playwright-report coverage .v8-coverage
