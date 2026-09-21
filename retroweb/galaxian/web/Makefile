# Build the WebAssembly module. Needs the Emscripten SDK on PATH.

EMCC ?= em++
CORE = ..
Z80 = ../../shared/cpu
SHARED = ../../shared
GAL = $(SHARED)/galaxian
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/sound.cpp \
      $(Z80)/cpu_z80.cpp $(GAL)/video.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CORE)/sound.h $(Z80)/cpu_z80.h \
      $(GAL)/video.h hwtest_roms.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=GalaxianArcade \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sSTACK_SIZE=1MB

galaxian.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) -I. -I$(CORE) -I$(Z80) -I$(SHARED) $(SRC) -o $@

hwtest_roms.h: $(CORE)/roms/hwtest/gen_hwtest.py
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h

.PHONY: shared roms wasm serve test test-install clean
wasm: galaxian.js
roms: hwtest_roms.h
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h \
	  --png ../../assets/galaxian-cabinet.png

shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js \
	   $(SHARED)/arcade-cab.css $(SHARED)/gdrive.js \
	   $(SHARED)/cab-start-1p-yellow.png $(SHARED)/cab-start-2p-yellow.png \
	   $(SHARED)/cab-start-1p-white.png $(SHARED)/cab-start-2p-white.png \
	   shared/

serve: galaxian.js roms shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: galaxian.js roms shared
	npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f galaxian.js galaxian.wasm hwtest_roms.h
	rm -rf roms shared
	rm -rf test-results playwright-report coverage .v8-coverage
