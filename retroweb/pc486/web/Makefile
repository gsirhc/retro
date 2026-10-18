# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

# em++ not emcc: embind needs libc++abi typeinfo the C driver won't link.
EMCC ?= em++

# DEBUG_PERF=1 compiles in the perf counters and the Debug panel. Off by default.
# The Performance panel's Tier 2 loads a second binary, pc486-perf.js/.wasm, via ?perf.
PERF ?= 0
ifeq ($(PERF),1)
EMCC_PERF_FLAGS = -DPC486_PERF
endif

CORE = ..
PC = ../../shared/pc
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/chipset.cpp $(CORE)/cpu80486.cpp $(CORE)/cache486.cpp \
      $(PC)/pic8259.cpp $(PC)/pit8253.cpp $(CORE)/i8042.cpp $(PC)/cmos_rtc.cpp \
      $(PC)/dma8237.cpp $(PC)/fdc765.cpp $(CORE)/ega.cpp $(CORE)/ega_render.cpp \
      $(CORE)/wd1003.cpp $(CORE)/atapi_cdrom.cpp $(PC)/pcspeaker.cpp \
      $(CORE)/soundblaster.cpp \
      $(CORE)/opl3.cpp \
      $(CORE)/mpu401.cpp

# Exception catching is required: the CPU core throws cpu80486::Fault for protected-mode faults.
# -fwasm-exceptions, not -fexceptions: step() wraps every instruction in try/catch, and JS-emulated
# unwinding is too slow to hold real time. Needs -sSUPPORT_LONGJMP=wasm to match.
# -O3 -flto: the hot loop spans several translation units (~10% throughput, PC486_REVIEW.md §8).
EMFLAGS = -O3 -flto -std=c++17 -lembind --no-entry -fwasm-exceptions -sSUPPORT_LONGJMP=wasm \
          $(EMCC_PERF_FLAGS) \
  -sMODULARIZE=1 -sEXPORT_NAME=Pc486 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

pc486.js: $(SRC)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@
	$(call write_deps,$(EMCC_PERF_FLAGS) $(SRC))

# Instrumented twin: same sources and export name, counters compiled in, chosen by ?perf.
pc486-perf.js: $(SRC)
	$(EMCC) $(EMFLAGS) -DPC486_PERF $(SRC) -o $@
	$(call write_deps,-DPC486_PERF $(SRC))

perf-build: pc486-perf.js

.PHONY: library-images shared roms hdd-image preset-images legacy-hdd-fixture driver-images perf-build serve test test-smoke test-install coverage clean stage

SHARED = ../../shared
roms:
	$(MAKE) -C $(CORE) bios
	mkdir -p roms
	cp $(CORE)/roms/BIOS-bochs-legacy $(CORE)/roms/VGABIOS-lgpl-latest.bin roms/

$(CORE)/disks/freedos-hdd.img:
	$(MAKE) -C $(CORE) hdd-image

disks/freedos-hdd.img: $(CORE)/disks/freedos-hdd.img
	mkdir -p disks
	cp $(CORE)/disks/freedos-hdd.img disks/

# Mostly-empty FAT16 gzips to a few MB. Only the .gz ships (stage drops the raw image); app.js
# decompresses it.
disks/freedos-hdd.img.gz: disks/freedos-hdd.img
	gzip -9 -k -f disks/freedos-hdd.img

hdd-image: disks/freedos-hdd.img disks/freedos-hdd.img.gz

PRESETS = doom wolf3d duke3d rott pinball tyrian
.PRECIOUS: $(CORE)/disks/%-hdd.img

$(CORE)/disks/%-hdd.img:
	$(MAKE) -C $(CORE) preset-hdds

disks/presets/%-hdd.img: $(CORE)/disks/%-hdd.img
	mkdir -p disks/presets
	cp $< $@

disks/presets/%-hdd.img.gz: disks/presets/%-hdd.img
	gzip -9 -k -f $<

preset-images: $(foreach p,$(PRESETS),disks/presets/$(p)-hdd.img disks/presets/$(p)-hdd.img.gz)

# Test-only C: images in the old 504MB format, for hdd.spec.ts (tests/make-legacy-hdd.sh).
tests/media/legacy-504.img: disks/freedos-hdd.img tests/make-legacy-hdd.sh
	bash tests/make-legacy-hdd.sh disks/freedos-hdd.img $@

tests/media/legacy-504-full.img: disks/freedos-hdd.img tests/make-legacy-hdd.sh
	bash tests/make-legacy-hdd.sh --full disks/freedos-hdd.img $@

tests/media/%.img.gz: tests/media/%.img
	gzip -9 -k -f $<

legacy-hdd-fixture: tests/media/legacy-504.img tests/media/legacy-504.img.gz tests/media/legacy-504-full.img.gz

# Fetched at runtime, so it lives under web/ (nothing one level up is staged).
$(CORE)/disks/ctmouse.img:
	$(MAKE) -C $(CORE) disks/ctmouse.img

disks/ctmouse.img: $(CORE)/disks/ctmouse.img
	mkdir -p disks
	cp $(CORE)/disks/ctmouse.img disks/

driver-images: disks/ctmouse.img

# Port 8000 like every machine; the Playwright suite uses 8400/8410.
serve: pc486.js pc486-perf.js roms shared driver-images
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 ../../serve_nocache.py 8000 .

# Playwright suite. Needs the same roms and images as serve, plus npm ci once. Ports 8400/8410.
test: pc486.js pc486-perf.js roms shared driver-images legacy-hdd-fixture
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@if [ ! -f disks/freedos-hdd.img.gz ]; then \
	  echo "disks/freedos-hdd.img.gz not found -- run 'make hdd-image' first"; \
	  echo "(app.js loads this file; the raw image is not deployed.)"; \
	  exit 1; \
	fi
	npx playwright test

# Smoke: tests/smoke.spec.ts only, what deploy CI runs.
test-smoke: pc486.js pc486-perf.js roms shared driver-images
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@if [ ! -f disks/freedos-hdd.img.gz ]; then \
	  echo "disks/freedos-hdd.img.gz not found -- run 'make hdd-image' first"; \
	  echo "(app.js loads this file; the raw image is not deployed.)"; \
	  exit 1; \
	fi
	npx playwright test tests/smoke.spec.ts

# Same suite, one worker, V8 coverage for app.js -> coverage/index.html (+ coverage/lcov.info)
coverage: pc486.js roms shared legacy-hdd-fixture
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  exit 1; \
	fi
	COVERAGE=1 npx playwright test

clean:
	rm -f pc486.js pc486.wasm pc486.d pc486-perf.d
	rm -rf shared
	rm -rf test-results playwright-report coverage .v8-coverage tests/media

# Fetched at runtime, so it lives under web/ (nothing one level up is staged).
library-images:
	$(MAKE) -C $(CORE) library
	mkdir -p disks/library
	cp $(CORE)/disks/library/*.img $(CORE)/disks/library.json disks/library/

STAGE = index.html app.js pc486.js pc486.wasm pc486-perf.js pc486-perf.wasm assets disks roms shared
SHARED_EXTRA = $(SHARED_PC)
define STAGE_POST
find $(DEST)/disks -name '*-hdd.img' -delete
python3 ../../strip_fast_flag.py $(DEST)/app.js
endef

include $(SHARED)/web.mk
