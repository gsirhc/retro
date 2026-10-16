# Top-level targets for the IBM PC/AT (5170-339) emulator.
#
#   make test        build + run the GoogleTest suite
#   make bios        fetch + verify the freely-licensed BIOS, assemble the EGA BIOS (needs nasm)
#   make bios-check   boot the real BIOS against this chipset natively, report progress
#   make hdd-image   build the shipped FreeDOS HDD image (see below, SLOW)
#   make preset-hdds build the Commander Keen preset C: (Apogee's installer run on a bare FreeDOS)
#   make render_screen   build the screen-to-BMP diagnostic tool (see below)
#   make wasm        build the WebAssembly module (needs emsdk, see web/Makefile)
#   make serve       build the wasm module + serve the browser front end
#   make coverage    build + run the unit tests with llvm-cov, print a report
#   make test-smoke  GoogleTest (what deploy CI runs; same as test today)
#   make check       what nightly runs
#   make clean       remove all build output
#
# `make` with no target prints this list.
#
# hdd-image runs the real FreeDOS 1.3 installer end to end (disks/build_freedos_hdd.cpp,
# IBM_PCAT_REVIEW.md §11). It takes minutes, so it stays out of check/test.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CHIPSET_SRCS = machine.cpp chipset.cpp cpu80286.cpp ../shared/pc/pic8259.cpp ../shared/pc/pit8253.cpp i8042.cpp ../shared/pc/cmos_rtc.cpp ../shared/pc/dma8237.cpp ../shared/pc/fdc765.cpp ega.cpp wd1003.cpp ../shared/pc/pcspeaker.cpp
UNIT_TESTS = cpu80286_test cpu80286_timing_test pic8259_test pit8253_test i8042_test cmos_rtc_test dma8237_test chipset_test machine_test fdc765_test ega_test ega_render_test wd1003_test pcspeaker_test egabios_test

.PHONY: help test test-smoke bios egabios bios-check library hdd-image preset-hdds wasm serve coverage check clean

LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- unit tests ---------------------------------------------------------
test: egabios
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- freely-licensed BIOS (IBM_PCAT_REVIEW.md §6) ---
bios:
	bash roms/fetch-bios.sh
	$(MAKE) -C roms/egabios

egabios:
	$(MAKE) -C roms/egabios

bios_host: bios_host.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ bios_host.cpp $(CHIPSET_SRCS)

bios-check: bios bios_host
	./bios_host roms/BIOS-bochs-legacy

# --- screen-to-BMP diagnostic (dev tool, not part of check/test) ---
render_screen: render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)

# --- shipped HDD image (slow, not part of check/test) ---
disks/build_freedos_hdd: disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)

hdd-image: bios disks/build_freedos_hdd
	bash disks/fetch-freedos.sh
	bash disks/fetch-freedos-install-set.sh
	./disks/build_freedos_hdd roms/BIOS-bochs-legacy roms/egabios/egabios.bin disks disks/freedos-hdd.img

# --- webassembly + browser front end ---
# Library floppies (disks/library.json): FreeDOS packages from pc486's pinned LiveCD, pinned shareware.
DOSLIB = ../shared/dos-library
FDCD = ../pc486/disks/freedos-cd.iso
$(FDCD):
	$(MAKE) -C ../pc486 disks/freedos-cd.iso

library: $(FDCD)
	bash $(DOSLIB)/fetch-shareware.sh
	python3 $(DOSLIB)/build-library.py --library disks/library.json --iso $(FDCD) --out disks/library

# --- preset C: (?disk=keen1), from the library's install floppy ---
disks/build_preset_hdd: disks/build_preset_hdd.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/build_preset_hdd.cpp $(CHIPSET_SRCS)

disks/library/keen1.img:
	$(MAKE) library

disks/keen1-hdd.img: disks/build_preset_hdd disks/build-preset-hdd.sh ../pc486/disks/zero-free-clusters.py disks/freedos-hdd.img disks/library/keen1.img
	bash disks/build-preset-hdd.sh keen1 ./disks/build_preset_hdd roms/BIOS-bochs-legacy roms/egabios/egabios.bin \
	  disks/freedos-hdd.img $@.tmp disks/library/keen1.img
	mv $@.tmp $@

preset-hdds: bios egabios disks/keen1-hdd.img

wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

# --- C++ coverage (source-based, llvm-cov) ------------------------------
coverage: egabios
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in $(UNIT_TESTS); do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  objs=""; for t in $(UNIT_TESTS); do objs="$$objs -object $$t"; done; objs=$${objs# -object}; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  srcs="$(CURDIR)/cpu80286.cpp $(CURDIR)/cpu80286.h $(CURDIR)/../shared/pc/pic8259.cpp $(CURDIR)/../shared/pc/pic8259.h \
	    $(CURDIR)/../shared/pc/pit8253.cpp $(CURDIR)/../shared/pc/pit8253.h $(CURDIR)/i8042.cpp $(CURDIR)/i8042.h \
	    $(CURDIR)/../shared/pc/cmos_rtc.cpp $(CURDIR)/../shared/pc/cmos_rtc.h $(CURDIR)/../shared/pc/dma8237.cpp $(CURDIR)/../shared/pc/dma8237.h \
	    $(CURDIR)/../shared/pc/fdc765.cpp $(CURDIR)/../shared/pc/fdc765.h $(CURDIR)/ega.cpp $(CURDIR)/ega.h \
	    $(CURDIR)/ega_render.cpp $(CURDIR)/ega_render.h \
	    $(CURDIR)/wd1003.cpp $(CURDIR)/wd1003.h \
	    $(CURDIR)/../shared/pc/pcspeaker.cpp $(CURDIR)/../shared/pc/pcspeaker.h \
	    $(CURDIR)/chipset.cpp $(CURDIR)/chipset.h $(CURDIR)/machine.cpp $(CURDIR)/machine.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/ibmpc-at/tests/coverage-cpp/index.html"

# --- aggregate ------------------------------------------------------
test-smoke: test

check: test

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp bios_host disks/build_freedos_hdd disks/build_preset_hdd render_screen
	$(MAKE) -C roms/egabios clean
	$(MAKE) -C web clean
