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

# Default ROM, built from cpu6502/rom/. Phony, since it has no prerequisites to timestamp against.
roms/firmware.bin:
	$(MAKE) -C $(CORE) rom
	mkdir -p roms
	cp $(CORE)/../../cpu6502/rom/tmp/firmware.bin roms/firmware.bin

# <addr>R entry addresses from the build's label file
roms/entrypoints.js: roms/firmware.bin
	python3 gen_entrypoints.py $(CORE)/../../cpu6502/rom/tmp/firmware.lbl roms/entrypoints.js

# gen_example_bin: host-compiled build tool, never shipped (see gen_example_bin.cpp)
CXX ?= c++
CORE_SRCS = $(CORE)/machine.cpp $(CORE)/bus.cpp $(CORE)/cpu65c02.cpp \
      $(CORE)/via65c22.cpp $(CORE)/acia65c51.cpp $(CORE)/eeprom28c256.cpp $(CORE)/hd44780.cpp
gen_example_bin: gen_example_bin.cpp $(CORE_SRCS) $(HDR)
	$(CXX) -std=c++17 -O2 -I$(CORE) -o $@ gen_example_bin.cpp $(CORE_SRCS)

# one .bin per examples/*.asm
EXAMPLE_SRCS := $(wildcard examples/*.asm)
EXAMPLE_BINS := $(EXAMPLE_SRCS:.asm=.bin)
examples/%.bin: examples/%.asm gen_example_bin roms/firmware.bin
	./gen_example_bin roms/firmware.bin $(CORE)/../../cpu6502/rom/tmp/firmware.lbl $< $@

.PHONY: shared wasm roms examples-bin roms/firmware.bin serve test test-smoke test-install clean
wasm: cgoac6502.js
roms: roms/firmware.bin roms/entrypoints.js
examples-bin: $(EXAMPLE_BINS)

# Shared front-end code, copied from retroweb/shared/ (git-ignored, regenerated)
SHARED = ../../shared
shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js \
	   shared/

serve: wasm roms examples-bin shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: wasm roms examples-bin shared
	npx playwright test

# smoke.spec.ts only (deploy CI); `make test` is the full suite
test-smoke: wasm roms examples-bin shared
	npx playwright test tests/smoke.spec.ts

coverage: wasm roms examples-bin shared
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f cgoac6502.js cgoac6502.wasm roms/firmware.bin gen_example_bin examples/*.bin
	rm -rf shared
	rm -rf test-results playwright-report coverage .v8-coverage
