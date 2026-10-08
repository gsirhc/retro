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
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/chipset.cpp $(CORE)/cpu80486.cpp $(CORE)/cache486.cpp \
      $(CORE)/pic8259.cpp $(CORE)/pit8253.cpp $(CORE)/i8042.cpp $(CORE)/cmos_rtc.cpp \
      $(CORE)/dma8237.cpp $(CORE)/fdc765.cpp $(CORE)/ega.cpp $(CORE)/ega_render.cpp \
      $(CORE)/wd1003.cpp $(CORE)/atapi_cdrom.cpp $(CORE)/pcspeaker.cpp \
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

# Headers count: hot paths are inline in the .h files, so a stale module hides header-only changes.
HDRS = $(wildcard $(CORE)/*.h)

pc486.js: $(SRC) $(HDRS)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

# Instrumented twin: same sources and export name, counters compiled in, chosen by ?perf.
pc486-perf.js: $(SRC) $(HDRS)
	$(EMCC) $(EMFLAGS) -DPC486_PERF $(SRC) -o $@

perf-build: pc486-perf.js

.PHONY: shared roms hdd-image legacy-hdd-fixture cdrom-image driver-images perf-build serve test test-smoke test-install coverage clean

# Copied from retroweb/shared/ at build time, git-ignored.
SHARED = ../../shared
shared:
	mkdir -p shared
	cp $(SHARED)/theme.css $(SHARED)/theme-init.js $(SHARED)/theme-picker.js $(SHARED)/pagebar.css \
	   $(SHARED)/fullscreen.css $(SHARED)/fullscreen.js \
	   $(SHARED)/focus-hint.css $(SHARED)/focus-hint.js \
	   $(SHARED)/footer.css $(SHARED)/footer.js $(SHARED)/winbtns.js \
	   shared/

roms:
	$(MAKE) -C $(CORE) bios
	mkdir -p roms
	cp $(CORE)/roms/BIOS-bochs-legacy $(CORE)/roms/VGABIOS-lgpl-latest.bin roms/

$(CORE)/disks/freedos-hdd.img:
	$(MAKE) -C $(CORE) hdd-image

disks/freedos-hdd.img: $(CORE)/disks/freedos-hdd.img
	mkdir -p disks
	cp $(CORE)/disks/freedos-hdd.img disks/

# Mostly-empty FAT16 gzips to a few MB. serve_nocache.py serves the .gz sibling transparently
# (Content-Encoding: gzip), so app.js fetch() is unchanged. freedos-cd.iso doesn't compress.
disks/freedos-hdd.img.gz: disks/freedos-hdd.img
	gzip -9 -k -f disks/freedos-hdd.img

hdd-image: disks/freedos-hdd.img disks/freedos-hdd.img.gz

# Test-only C: images in the old 504MB format, for hdd.spec.ts (tests/make-legacy-hdd.sh).
tests/media/legacy-504.img: disks/freedos-hdd.img tests/make-legacy-hdd.sh
	bash tests/make-legacy-hdd.sh disks/freedos-hdd.img $@

tests/media/legacy-504-full.img: disks/freedos-hdd.img tests/make-legacy-hdd.sh
	bash tests/make-legacy-hdd.sh --full disks/freedos-hdd.img $@

tests/media/%.img.gz: tests/media/%.img
	gzip -9 -k -f $<

legacy-hdd-fixture: tests/media/legacy-504.img tests/media/legacy-504.img.gz tests/media/legacy-504-full.img.gz

# Default mounted CD-ROM: FreeDOS's official live CD (../disks/fetch-freedos-cd.sh).
$(CORE)/disks/freedos-cd.iso:
	$(MAKE) -C $(CORE) disks/freedos-cd.iso

disks/freedos-cd.iso: $(CORE)/disks/freedos-cd.iso
	mkdir -p disks
	cp $(CORE)/disks/freedos-cd.iso disks/

# Fetched at runtime, so it lives under web/ (nothing one level up is staged).
$(CORE)/disks/ctmouse.img:
	$(MAKE) -C $(CORE) disks/ctmouse.img

disks/ctmouse.img: $(CORE)/disks/ctmouse.img
	mkdir -p disks
	cp $(CORE)/disks/ctmouse.img disks/

driver-images: disks/ctmouse.img

cdrom-image: disks/freedos-cd.iso

# Port 8000 like every machine; the Playwright suite uses 8400/8410.
serve: pc486.js pc486-perf.js roms shared cdrom-image driver-images
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

# Playwright suite. Needs the same roms and images as serve, plus npm ci once. Ports 8400/8410.
test: pc486.js pc486-perf.js roms shared cdrom-image driver-images legacy-hdd-fixture
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@if [ ! -f disks/freedos-hdd.img.gz ]; then \
	  echo "disks/freedos-hdd.img.gz not found -- run 'make hdd-image' first"; \
	  echo "(Playwright's devserve.py serves this sibling as Content-Encoding: gzip;"; \
	  echo " without it every remount transfers the raw 256MB image.)"; \
	  exit 1; \
	fi
	npx playwright test

# Smoke: tests/smoke.spec.ts only, what deploy CI runs.
test-smoke: pc486.js pc486-perf.js roms shared cdrom-image driver-images
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(NOT YET IMPLEMENTED for this machine -- see ../Makefile's MILESTONE 1 STATUS note)."; \
	  exit 1; \
	fi
	@if [ ! -f disks/freedos-hdd.img.gz ]; then \
	  echo "disks/freedos-hdd.img.gz not found -- run 'make hdd-image' first"; \
	  echo "(Playwright's devserve.py serves this sibling as Content-Encoding: gzip;"; \
	  echo " without it every remount transfers the raw 256MB image.)"; \
	  exit 1; \
	fi
	npx playwright test tests/smoke.spec.ts

# Same suite, one worker, V8 coverage for app.js -> coverage/index.html (+ coverage/lcov.info)
coverage: pc486.js roms shared cdrom-image legacy-hdd-fixture
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  exit 1; \
	fi
	COVERAGE=1 npx playwright test

test-install:
	npm ci
	npx playwright install --with-deps chromium

clean:
	rm -f pc486.js pc486.wasm
	rm -rf shared
	rm -rf test-results playwright-report coverage .v8-coverage tests/media
