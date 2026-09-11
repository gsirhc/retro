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

# gen_example_bin -- native (host-compiler) build tool, never shipped:
# assembles each Example program by actually running the board's own
# real two-pass assembler headlessly, once, so the browser can poke the
# result straight into RAM instead of re-running that assembly (over a
# simulated serial line, character by character) on every visitor's own
# machine -- see gen_example_bin.cpp's own header for the full why.
# Built with the *host's* C++ compiler, not emcc -- this never runs in
# the browser. CXX defaults to c++ (matches how CMake's own tests build).
CXX ?= c++
CORE_SRCS = $(CORE)/machine.cpp $(CORE)/bus.cpp $(CORE)/cpu65c02.cpp \
      $(CORE)/via65c22.cpp $(CORE)/acia65c51.cpp $(CORE)/eeprom28c256.cpp $(CORE)/hd44780.cpp
gen_example_bin: gen_example_bin.cpp $(CORE_SRCS) $(HDR)
	$(CXX) -std=c++17 -O2 -I$(CORE) -o $@ gen_example_bin.cpp $(CORE_SRCS)

# One .bin per examples/*.asm -- see gen_example_bin.cpp's header for the
# binary format app.js's loadExampleBinary() expects.
EXAMPLE_SRCS := $(wildcard examples/*.asm)
EXAMPLE_BINS := $(EXAMPLE_SRCS:.asm=.bin)
examples/%.bin: examples/%.asm gen_example_bin roms/firmware.bin
	./gen_example_bin roms/firmware.bin $(CORE)/../../cpu6502/rom/tmp/firmware.lbl $< $@

.PHONY: wasm roms examples-bin roms/firmware.bin serve test test-install clean
wasm: cgoac6502.js
roms: roms/firmware.bin roms/entrypoints.js
examples-bin: $(EXAMPLE_BINS)

serve: wasm roms examples-bin
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: wasm roms examples-bin
	npx playwright test

coverage: wasm roms examples-bin
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f cgoac6502.js cgoac6502.wasm roms/firmware.bin gen_example_bin examples/*.bin
	rm -rf test-results playwright-report coverage .v8-coverage
