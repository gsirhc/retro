# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

# Use the C++ driver (em++), not emcc -- embind pulls in libc++abi typeinfo
# that the C driver won't link.
EMCC ?= em++

CORE = ..
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/chipset.cpp $(CORE)/cpu80286.cpp \
      $(CORE)/pic8259.cpp $(CORE)/pit8253.cpp $(CORE)/i8042.cpp $(CORE)/cmos_rtc.cpp \
      $(CORE)/dma8237.cpp $(CORE)/fdc765.cpp $(CORE)/ega.cpp $(CORE)/ega_render.cpp \
      $(CORE)/wd1003.cpp $(CORE)/pcspeaker.cpp

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=IbmPcAt \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

ibmpcat.js: $(SRC)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

.PHONY: roms hdd-image serve clean
# Firmware + shipped HDD image are fetched/built via the native-core
# Makefile's own targets (single source of truth for pinning/verification,
# see ../roms/fetch-bios.sh and ../disks/build_freedos_hdd.cpp), then
# copied into web/roms, web/disks -- the deployed site only ever stages
# this directory (see CLAUDE.md), so nothing the page fetches at runtime
# can live one level up via "../". `bios`'s own recipe is cheap/idempotent
# (fetch-bios.sh checksums and skips what's already correct) so re-running
# it is fine; `hdd-image`'s is NOT (a real multi-minute installer run), so
# that one is gated on the parent's own output file actually being
# missing, not re-invoked unconditionally on every `make serve`.
roms:
	$(MAKE) -C $(CORE) bios
	mkdir -p roms
	cp $(CORE)/roms/BIOS-bochs-legacy $(CORE)/roms/VGABIOS-lgpl-latest.bin roms/

$(CORE)/disks/freedos-hdd.img:
	$(MAKE) -C $(CORE) hdd-image

disks/freedos-hdd.img: $(CORE)/disks/freedos-hdd.img
	mkdir -p disks
	cp $(CORE)/disks/freedos-hdd.img disks/

hdd-image: disks/freedos-hdd.img

# Port 8000, matching altair8800/assembler6502's own `make serve` default --
# each machine's Playwright suite (Phase 8 here) reserves its own dedicated
# port instead (8100/8200 there) precisely so it never collides with a
# hand-run `make serve`; this machine's suite should follow that same
# pattern rather than reusing 8000.
serve: ibmpcat.js roms
	@if [ ! -f disks/freedos-hdd.img ]; then \
	  echo "disks/freedos-hdd.img not found -- run 'make hdd-image' first"; \
	  echo "(builds the shipped FreeDOS image by running the real installer; takes real minutes, run once)."; \
	  exit 1; \
	fi
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

clean:
	rm -f ibmpcat.js ibmpcat.wasm
