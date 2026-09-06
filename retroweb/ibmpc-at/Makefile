# Top-level "scripts" for the IBM PC/AT (5170-339) emulator. Each target is
# a no-arg command:
#
#   make test        build + run the GoogleTest suite
#   make bios        fetch + verify the freely-licensed BIOS/video-BIOS images
#   make bios-check   boot the real BIOS against this chipset natively, report progress
#   make hdd-image   build the shipped FreeDOS HDD image (see below -- SLOW)
#   make render_screen   build the screen-to-BMP diagnostic tool (see below)
#   make coverage    build + run the unit tests with llvm-cov, print a report
#   make check       what CI would run
#   make clean       remove all build output
#
# `make` with no target prints this list.
#
# `make hdd-image` is deliberately NOT part of `make check`/`test`: it
# builds this machine's shipped hard disk image by actually running the
# real FreeDOS 1.3 installer against the emulator end to end (see
# disks/build_freedos_hdd.cpp and IBM_PCAT_REVIEW.md §11), which takes
# real minutes (tens of billions of emulated 8MHz cycles, every floppy
# read paced to its genuine transfer rate) -- run it explicitly, once,
# whenever disks/freedos-hdd.img needs (re)building.
#
# NOTE: this machine is being built in phases (see IBM_PCAT_REVIEW.md and the
# approved plan). There is no WASM/browser build yet, so `wasm`/`serve`
# targets (present on the other two retroweb machines) aren't added until
# Phase 7/8 land. Don't add stub targets ahead of the code they'd invoke.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CHIPSET_SRCS = machine.cpp chipset.cpp cpu80286.cpp pic8259.cpp pit8253.cpp i8042.cpp cmos_rtc.cpp dma8237.cpp fdc765.cpp ega.cpp wd1003.cpp
UNIT_TESTS = cpu80286_test pic8259_test pit8253_test i8042_test cmos_rtc_test dma8237_test chipset_test machine_test fdc765_test ega_test wd1003_test

.PHONY: help test bios bios-check hdd-image coverage check clean

LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- unit tests ---------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- freely-licensed firmware (substitutes for IBM's still-copyrighted AT
# BIOS / EGA video BIOS -- see IBM_PCAT_REVIEW.md §6) -------------------
bios:
	bash roms/fetch-bios.sh

bios_host: bios_host.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ bios_host.cpp $(CHIPSET_SRCS)

bios-check: bios bios_host
	./bios_host roms/BIOS-bochs-legacy

# --- screen-to-BMP diagnostic (see render_screen.cpp) -- boots the real
# BIOS/vgabios/optional disks for a cycle budget, then renders the current
# EGA text-mode screen using the real character-generator RAM and palette
# registers. Not gated by `check`/`test` (it's a dev tool, not a behavior
# to regress-test -- same reasoning as bios_host.cpp having no test file of
# its own); usage varies per invocation so there's no fixed "-check" target
# for it either. -------------------------------------------------------
render_screen: render_screen.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ render_screen.cpp $(CHIPSET_SRCS)

# --- shipped HDD image: install the real FreeDOS 1.3 installer end to end
# (see disks/build_freedos_hdd.cpp and IBM_PCAT_REVIEW.md §11) -- slow, not
# part of `check`/`test`. ------------------------------------------------
disks/build_freedos_hdd: disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)

hdd-image: bios disks/build_freedos_hdd
	bash disks/fetch-freedos.sh
	bash disks/fetch-freedos-install-set.sh
	./disks/build_freedos_hdd roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin disks disks/freedos-hdd.img

# --- C++ coverage (source-based, llvm-cov) ------------------------------
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in $(UNIT_TESTS); do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  objs=""; for t in $(UNIT_TESTS); do objs="$$objs -object $$t"; done; objs=$${objs# -object}; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  srcs="$(CURDIR)/cpu80286.cpp $(CURDIR)/cpu80286.h $(CURDIR)/pic8259.cpp $(CURDIR)/pic8259.h \
	    $(CURDIR)/pit8253.cpp $(CURDIR)/pit8253.h $(CURDIR)/i8042.cpp $(CURDIR)/i8042.h \
	    $(CURDIR)/cmos_rtc.cpp $(CURDIR)/cmos_rtc.h $(CURDIR)/dma8237.cpp $(CURDIR)/dma8237.h \
	    $(CURDIR)/fdc765.cpp $(CURDIR)/fdc765.h $(CURDIR)/ega.cpp $(CURDIR)/ega.h \
	    $(CURDIR)/wd1003.cpp $(CURDIR)/wd1003.h \
	    $(CURDIR)/chipset.cpp $(CURDIR)/chipset.h $(CURDIR)/machine.cpp $(CURDIR)/machine.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/ibmpc-at/tests/coverage-cpp/index.html"

# --- aggregate ------------------------------------------------------
check: test

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp bios_host disks/build_freedos_hdd render_screen
