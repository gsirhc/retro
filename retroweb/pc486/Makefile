# Targets for the 486 DX2-66 Gaming PC emulator (PC486_REVIEW.md).
#
#   make test        build + run the GoogleTest suite
#   make bios        fetch + verify the freely-licensed BIOS/video-BIOS images
#   make bios-check   boot the real BIOS against this chipset natively, report progress
#   make pm-check    run the protected-mode/paging/FPU end-to-end stub proof
#   make vbe-check   run the VGA mode 13h / DAC / VESA end-to-end pixel proof
#   make hdd-image   build the shipped FreeDOS HDD image (see below -- SLOW)
#   make hdd-boot-check  cold-boot the finished HDD image standalone, report the screen
#   make render_screen   build the screen-to-BMP diagnostic tool (see below)
#   make fm-trace-render build the OPL3 trace-to-WAV dev tool (see below)
#   make wasm        build the WebAssembly module (needs emsdk -- see web/Makefile)
#   make serve       build the wasm module + serve the browser front end
#   make coverage    build + run the unit tests with llvm-cov, print a report
#   make test-smoke  GoogleTest (what deploy CI runs)
#   make check       test + pm-check + vbe-check (what nightly runs)
#   make clean       remove all build output
#
# `make` with no target prints this list.
#

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CHIPSET_SRCS = machine.cpp chipset.cpp cpu80486.cpp cache486.cpp pic8259.cpp pit8253.cpp i8042.cpp cmos_rtc.cpp dma8237.cpp fdc765.cpp ega.cpp wd1003.cpp atapi_cdrom.cpp pcspeaker.cpp soundblaster.cpp opl3.cpp mpu401.cpp
UNIT_TESTS = cpu80486_test cpu80486_timing_test cache486_test pic8259_test pit8253_test i8042_test cmos_rtc_test dma8237_test chipset_test machine_test fdc765_test ega_test ega_render_test wd1003_test atapi_cdrom_test pcspeaker_test soundblaster_test mpu401_test opl3_test

.PHONY: help test test-smoke bios bios-check pm-check vbe-check hdd-image hdd-boot-check wasm serve coverage check clean fm-trace-render

LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- unit tests ---------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- freely-licensed firmware ---
# Generic Bochs-legacy BIOS substitute, same one ibmpc-at pins.
bios:
	bash roms/fetch-bios.sh

bios_host: bios_host.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ bios_host.cpp $(CHIPSET_SRCS)

bios-check: bios bios_host
	./bios_host roms/BIOS-bochs-legacy

# --- protected-mode end-to-end proof (pm_stub_check.cpp) ---
# Runs a DOS4GW-style stub: flat 32-bit entry, paging, #PF restart, task switch,
# ring 3, x87, return to real mode. Fast and image-free, so part of `check`.
pm_stub_check: pm_stub_check.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ pm_stub_check.cpp $(CHIPSET_SRCS)

pm-check: pm_stub_check
	./pm_stub_check

# --- VGA mode 13h / DAC / VESA proof (vbe_mode13_check.cpp) ---
# Boots a hand-assembled boot sector and checks the 320x200 and 640x400 pictures
# pixel by pixel. Needs the firmware images, so it depends on `bios`.
vbe_mode13_check: vbe_mode13_check.cpp ega_render.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ vbe_mode13_check.cpp ega_render.cpp $(CHIPSET_SRCS)

vbe-check: bios vbe_mode13_check
	./vbe_mode13_check roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin

# --- screen-to-BMP diagnostic (see render_screen.cpp) -------------------
render_screen: render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)

# --- OPL3 trace-to-WAV dev tool (fm_trace_render.cpp) ---
# Replays a `?fmtrace` capture through Opl3 alone. Needs only opl3.cpp/.h.
fm_trace_render: fm_trace_render.cpp opl3.cpp opl3.h
	$(CXX) $(CXXFLAGS) -o $@ fm_trace_render.cpp opl3.cpp

fm-trace-render: fm_trace_render

# --- FreeDOS LiveCD ISO + companion boot floppy ---
# The default CD-ROM disc. Install boots from floppy because the ISO's El Torito
# image is ISOLINUX + MEMDISK, which needs protected mode (PC486_REVIEW.md §5.1).
# Both files come from one verified zip (fetch-freedos-cd.sh).
disks/freedos-cd.iso disks/freedos-boot.img:
	bash disks/fetch-freedos-cd.sh

# --- shipped HDD image ---
# Not part of `check`/`test`: it runs the real FreeDOS 1.3 installer (real minutes;
# build_freedos_hdd.cpp, PC486_REVIEW.md §5). Exits non-zero without writing if
# the installer doesn't finish, so a partial image is never accepted.
disks/build_freedos_hdd: disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)

hdd-image: bios disks/build_freedos_hdd disks/freedos-cd.iso disks/freedos-boot.img
	./disks/build_freedos_hdd roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin \
	  disks/freedos-boot.img disks/freedos-cd.iso disks/freedos-hdd.img
	bash disks/apply-dos-config.sh disks/freedos-hdd.img

# CuteMouse driver floppy for the Drivers panel, built from files already on
# the HDD image (see disks/build-ctmouse-floppy.sh for the checksum it enforces)
disks/ctmouse.img: disks/build-ctmouse-floppy.sh disks/freedos-hdd.img
	bash disks/build-ctmouse-floppy.sh

ctmouse-image: disks/ctmouse.img

# --- boot the finished HDD image standalone (hdd_boot_check.cpp) ---
hdd-boot-check: bios disks/hdd_boot_check
	./disks/hdd_boot_check roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin disks/freedos-hdd.img

disks/hdd_boot_check: disks/hdd_boot_check.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/hdd_boot_check.cpp $(CHIPSET_SRCS)

# --- webassembly + browser front end (see web/) -------------------------
wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

# --- C++ coverage (source-based, llvm-cov) ------------------------------
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in $(UNIT_TESTS); do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  objs=""; for t in $(UNIT_TESTS); do objs="$$objs -object $$t"; done; objs=$${objs# -object}; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  srcs="$(CURDIR)/cpu80486.cpp $(CURDIR)/cpu80486.h $(CURDIR)/cache486.cpp $(CURDIR)/cache486.h $(CURDIR)/pic8259.cpp $(CURDIR)/pic8259.h \
	    $(CURDIR)/pit8253.cpp $(CURDIR)/pit8253.h $(CURDIR)/i8042.cpp $(CURDIR)/i8042.h \
	    $(CURDIR)/cmos_rtc.cpp $(CURDIR)/cmos_rtc.h $(CURDIR)/dma8237.cpp $(CURDIR)/dma8237.h \
	    $(CURDIR)/fdc765.cpp $(CURDIR)/fdc765.h $(CURDIR)/ega.cpp $(CURDIR)/ega.h \
	    $(CURDIR)/ega_render.cpp $(CURDIR)/ega_render.h \
	    $(CURDIR)/wd1003.cpp $(CURDIR)/wd1003.h $(CURDIR)/atapi_cdrom.cpp $(CURDIR)/atapi_cdrom.h \
	    $(CURDIR)/pcspeaker.cpp $(CURDIR)/pcspeaker.h \
	    $(CURDIR)/soundblaster.cpp $(CURDIR)/soundblaster.h \
	    $(CURDIR)/opl3.cpp $(CURDIR)/opl3.h \
	    $(CURDIR)/mpu401.cpp $(CURDIR)/mpu401.h \
	    $(CURDIR)/chipset.cpp $(CURDIR)/chipset.h $(CURDIR)/machine.cpp $(CURDIR)/machine.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/pc486/tests/coverage-cpp/index.html"

# --- aggregate ------------------------------------------------------
test-smoke: test

check: test pm-check vbe-check

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp bios_host pm_stub_check vbe_mode13_check disks/build_freedos_hdd disks/hdd_boot_check disks/boom_run_check render_screen fm_trace_render
	$(MAKE) -C web clean
