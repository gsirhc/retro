# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

EMCC ?= em++

CORE = ..
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/bus.cpp $(CORE)/cpu65c02.cpp \
      $(CORE)/via65c22.cpp $(CORE)/acia65c51.cpp $(CORE)/eeprom28c256.cpp $(CORE)/hd44780.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CORE)/bus.h $(CORE)/cpu65c02.h \
      $(CORE)/via65c22.h $(CORE)/acia65c51.h $(CORE)/eeprom28c256.h $(CORE)/hd44780.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=CgOac6502 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

cgoac6502.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

# The default ROM, built from your own source (cpu6502/rom/), not fetched.
# .PHONY (below) -- this target has no file prerequisites of its own to
# compare timestamps against, so without being phony, make treats the
# mere *existence* of roms/firmware.bin as "up to date" forever and never
# reruns this recipe again, even though the inner `rom` target (../Makefile)
# is itself unconditional -- a real staleness bug this repo hit once
# already (see retroweb/README.md's Cache-Control note for the browser-
# side half of the same class of problem).
roms/firmware.bin:
	$(MAKE) -C $(CORE) rom
	mkdir -p roms
	cp $(CORE)/../../cpu6502/rom/tmp/firmware.bin roms/firmware.bin

# The editor/assembler/load/save <addr>R entry addresses, extracted from
# the same build's own label file -- see gen_entrypoints.py's header.
roms/entrypoints.js: roms/firmware.bin
	python3 gen_entrypoints.py $(CORE)/../../cpu6502/rom/tmp/firmware.lbl roms/entrypoints.js

.PHONY: wasm roms roms/firmware.bin serve test test-install clean
wasm: cgoac6502.js
roms: roms/firmware.bin roms/entrypoints.js

serve: wasm roms
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: wasm roms
	npx playwright test

coverage: wasm roms
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f cgoac6502.js cgoac6502.wasm roms/firmware.bin
	rm -rf test-results playwright-report coverage .v8-coverage
