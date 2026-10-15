# Needs the Emscripten SDK on PATH (emsdk install latest, activate, source emsdk_env.sh).

# em++ not emcc: embind needs libc++abi typeinfo.
EMCC ?= em++

CORE = ..
PC = ../../shared/pc
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/chipset.cpp $(CORE)/cpu80286.cpp \
      $(PC)/pic8259.cpp $(PC)/pit8253.cpp $(CORE)/i8042.cpp $(PC)/cmos_rtc.cpp \
      $(PC)/dma8237.cpp $(CORE)/fdc765.cpp $(CORE)/ega.cpp $(CORE)/ega_render.cpp \
      $(CORE)/wd1003.cpp $(PC)/pcspeaker.cpp

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=IbmPcAt \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

HDRS = $(wildcard $(CORE)/*.h $(PC)/*.h)

ibmpcat.js: $(SRC) $(HDRS)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

.PHONY: library-images shared roms hdd-image serve test test-smoke test-install coverage clean preset-images stage

SHARED = ../../shared
# Firmware and the HDD image come from the native-core Makefile (pinned and verified there).
# hdd-image is a multi-minute installer run, so it only runs when the parent output is missing.
roms:
	$(MAKE) -C $(CORE) bios
	mkdir -p roms
	cp $(CORE)/roms/BIOS-bochs-legacy $(CORE)/roms/egabios/egabios.bin roms/

$(CORE)/disks/freedos-hdd.img:
	$(MAKE) -C $(CORE) hdd-image

disks/freedos-hdd.img: $(CORE)/disks/freedos-hdd.img
	mkdir -p disks
	cp $(CORE)/disks/freedos-hdd.img disks/

hdd-image: disks/freedos-hdd.img

$(CORE)/disks/keen1-hdd.img:
	$(MAKE) -C $(CORE) preset-hdds
.PRECIOUS: $(CORE)/disks/keen1-hdd.img

disks/presets/keen1-hdd.img: $(CORE)/disks/keen1-hdd.img
	mkdir -p disks/presets
	cp $< $@

disks/presets/keen1-hdd.img.gz: disks/presets/keen1-hdd.img
	gzip -9 -k -f $<

preset-images: disks/presets/keen1-hdd.img disks/presets/keen1-hdd.img.gz

# Port 8000 like the other machines; the Playwright suite uses its own port.
serve: ibmpcat.js roms shared
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(builds the shipped FreeDOS image by running the real installer; takes real minutes, run once)."; \
	  exit 1; \
	fi
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

# Playwright suite. Needs the same roms/hdd-image as serve, plus npm ci once.
# Ports 8300/8310 keep it clear of serve's 8000.
test: ibmpcat.js roms shared
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(builds the shipped FreeDOS image by running the real installer; takes real minutes, run once)."; \
	  exit 1; \
	fi
	npx playwright test

# Smoke: tests/smoke.spec.ts only (deploy CI). Full suite is make test.
test-smoke: ibmpcat.js roms shared
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(builds the shipped FreeDOS image by running the real installer; takes real minutes, run once)."; \
	  exit 1; \
	fi
	npx playwright test tests/smoke.spec.ts

# same suite, one worker, with V8 line/branch/function coverage for app.js
# -> coverage/index.html  (+ coverage/lcov.info)
coverage: ibmpcat.js roms shared
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  exit 1; \
	fi
	COVERAGE=1 npx playwright test

clean:
	rm -f ibmpcat.js ibmpcat.wasm
	rm -rf shared
	rm -rf test-results playwright-report coverage .v8-coverage

# Fetched at runtime, so it lives under web/ (nothing one level up is staged).
library-images:
	$(MAKE) -C $(CORE) library
	mkdir -p disks/library
	cp $(CORE)/disks/library/*.img $(CORE)/disks/library.json disks/library/

STAGE = index.html app.js ibmpcat.js ibmpcat.wasm assets disks roms shared
SHARED_EXTRA = $(SHARED_PC)
STAGE_POST = python3 ../../strip_fast_flag.py $(DEST)/app.js

include $(SHARED)/web.mk
