# Build the Asteroids WebAssembly module. Needs Emscripten on PATH.

EMCC ?= em++
CORE = ..
CPU = ../../shared/cpu
ATARI = ../../shared/atari
SHARED = ../../shared
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CPU)/cpu_mos6502.cpp $(ATARI)/dvg.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CPU)/cpu_mos6502.h $(ATARI)/dvg.h hwtest_roms.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=AsteroidsArcade \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sSTACK_SIZE=1MB

asteroids.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) -I. -I$(CORE) -I$(CPU) -I$(ATARI) -I$(SHARED) $(SRC) -o $@

hwtest_roms.h: $(CORE)/roms/hwtest/gen_hwtest.py
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h

.PHONY: shared roms wasm serve test test-smoke test-install clean
wasm: asteroids.js
roms: hwtest_roms.h
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h \
	  --png ../../assets/asteroids-cabinet.png

shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js \
	   $(SHARED)/arcade-cab.css \
	   $(SHARED)/cab-start-1p-yellow.png $(SHARED)/cab-start-2p-yellow.png \
	   $(SHARED)/cab-start-1p-white.png $(SHARED)/cab-start-2p-white.png \
	   shared/

serve: asteroids.js roms shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: asteroids.js roms shared
	npx playwright test

test-smoke: asteroids.js roms shared
	npx playwright test tests/smoke.spec.ts

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f asteroids.js asteroids.wasm hwtest_roms.h
	rm -rf roms shared
	rm -rf test-results playwright-report
