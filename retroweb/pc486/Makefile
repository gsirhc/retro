# Top-level "scripts" for the 486DX2-66 Gaming PC emulator -- a 1990s
# "gamer's dream" 486 build, not a recreation of a specific historical
# machine (see PC486_REVIEW.md). Each target is a no-arg command:
#
#   make test        build + run the GoogleTest suite
#   make bios        fetch + verify the freely-licensed BIOS/video-BIOS images
#   make bios-check   boot the real BIOS against this chipset natively, report progress
#   make pm-check    run the protected-mode/paging/FPU end-to-end stub proof
#   make vbe-check   run the VGA mode 13h / DAC / VESA end-to-end pixel proof
#   make hdd-image   build the shipped FreeDOS HDD image (see below -- SLOW)
#   make hdd-boot-check  cold-boot the finished HDD image standalone, report the screen
#   make render_screen   build the screen-to-BMP diagnostic tool (see below)
#   make wasm        build the WebAssembly module (needs emsdk -- see web/Makefile)
#   make serve       build the wasm module + serve the browser front end
#   make coverage    build + run the unit tests with llvm-cov, print a report
#   make check       what CI would run
#   make clean       remove all build output
#
# `make` with no target prints this list.
#
# MILESTONE 1 STATUS: the CPU core, chipset, floppy, ATAPI CD-ROM/HDD, and
# basic VGA text/graphics are implemented and unit-tested (`make test`), and
# `hdd-image` is real: it drives the genuine, unmodified FreeDOS 1.3
# installer end to end against this emulator -- booted off FreeDOS's own
# companion boot floppy with the LiveCD ISO mounted on the ATAPI CD-ROM,
# which is how the install actually sources every package (not ibmpc-at's
# multi-floppy swap flow). `hdd-boot-check` then cold-boots the finished
# image standalone to a live C:\> prompt. See PC486_REVIEW.md §5 for the
# boot-path decision and the emulator bugs this process found.
#
# MILESTONE 2 STATUS: the CPU core now implements protected mode,
# segmentation with a real GDT/LDT, paging with a TLB, protected-mode
# interrupts and gates, hardware task switching, and the on-die x87 FPU.
# `pm-check` is the end-to-end bar for that work -- a hand-assembled
# DOS4GW-style protected-mode stub run against this core -- and
# `hdd-boot-check` still reaches the same idle C:\> at the same cycle
# count, so real mode is unchanged. Virtual-8086 mode is still absent, so
# §5.9's FreeDOS boot-menu workaround stays. See PC486_REVIEW.md §6.
#
# MILESTONE 3 STATUS: the video card now does its genuinely-VGA modes --
# chain-4 mode 13h (320x200x256), the real 256-entry 6-bit DAC with its PEL
# mask, and the SVGA extension registers its own ROM drives to implement the
# VESA BIOS Extensions (INT 10h AX=4F00-4F03), including the 640x400x256 and
# 320x240x256 modes that fit in its 256KB. `vbe-check` is the end-to-end bar:
# a hand-assembled boot sector, booted off the real floppy controller by the
# real BIOS, whose pixels are then checked one by one. `hdd-boot-check` still
# reaches the same idle C:\>, so the text/real-mode path is unchanged. See
# PC486_REVIEW.md §7.
#
# MILESTONE 4 STATUS: a PS/2 mouse on the 8042's AUX port and a Sound
# Blaster 16 at 0x220/IRQ5/DMA1+5 are wired into the chipset and exposed to
# the browser front end (injectMouseEvent/sbDrainSamples in
# web/wasm_machine.cpp). Both were proved end to end against BOOM, a real
# DJGPP/CWSDPMI DOS Doom source port (PC486_REVIEW.md §9-§13); `boom-check`
# is gone now that the shipped HDD image installs FreeDOS's Base package
# set (no games -- see §19.5), so that proof is historical record, not a
# standing target. Ongoing coverage is the unit tests (i8042_test,
# soundblaster_test).

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CHIPSET_SRCS = machine.cpp chipset.cpp cpu80486.cpp pic8259.cpp pit8253.cpp i8042.cpp cmos_rtc.cpp dma8237.cpp fdc765.cpp ega.cpp wd1003.cpp atapi_cdrom.cpp pcspeaker.cpp soundblaster.cpp
UNIT_TESTS = cpu80486_test cpu80486_timing_test pic8259_test pit8253_test i8042_test cmos_rtc_test dma8237_test chipset_test machine_test fdc765_test ega_test ega_render_test wd1003_test atapi_cdrom_test pcspeaker_test soundblaster_test

.PHONY: help test bios bios-check pm-check vbe-check hdd-image hdd-boot-check wasm serve coverage check clean

LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- unit tests ---------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- freely-licensed firmware (substitutes for still-copyrighted original
# PC BIOS / VGA BIOS -- reuses the same generic Bochs-legacy substitute
# ibmpc-at pins, since it's a generic PC BIOS, not 80286-specific) --------
bios:
	bash roms/fetch-bios.sh

bios_host: bios_host.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ bios_host.cpp $(CHIPSET_SRCS)

bios-check: bios bios_host
	./bios_host roms/BIOS-bochs-legacy

# --- protected-mode end-to-end proof (see pm_stub_check.cpp) -------------
# Assembles and runs a minimal DOS4GW-style protected-mode stub against this
# core: LGDT/PE/far-JMP into a 32-bit flat segment, paging with a virtual
# alias, a deliberate #PF the handler repairs so the instruction restarts, a
# hardware task switch, a ring-3 excursion through a DPL-3 gate, the x87
# FPU, and the trip back to real mode. Exits non-zero if any of its own
# checks fail, so it works as a regression test as well as a demonstration.
# Fast (no images, no BIOS), so unlike hdd-boot-check it is part of `check`.
pm_stub_check: pm_stub_check.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ pm_stub_check.cpp $(CHIPSET_SRCS)

pm-check: pm_stub_check
	./pm_stub_check

# --- VGA mode 13h / DAC / VESA end-to-end proof (see vbe_mode13_check.cpp) -
# Boots a hand-assembled 512-byte boot sector off the real floppy controller
# with the real BIOS + video BIOS, and checks the resulting screen pixel by
# pixel: VBE 4F00-4F03, the DAC read/write ports, chain-4 addressing, and
# both the 320x200x256 and 640x400x256 pictures the card can actually show.
# Exits non-zero if any check fails, so like pm-check it doubles as a
# regression test. Needs the firmware images, so it depends on `bios`.
vbe_mode13_check: vbe_mode13_check.cpp ega_render.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ vbe_mode13_check.cpp ega_render.cpp $(CHIPSET_SRCS)

vbe-check: bios vbe_mode13_check
	./vbe_mode13_check roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin

# --- screen-to-BMP diagnostic (see render_screen.cpp) -------------------
render_screen: render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ render_screen.cpp ega_render.cpp $(CHIPSET_SRCS)

# --- FreeDOS LiveCD ISO + its companion boot floppy: this machine's
# default mounted CD-ROM disc, and the traditional "boot from floppy,
# install from CD" path FreeDOS itself ships FD13BOOT.img for. That's the
# path this machine uses, and not merely out of caution: the ISO's El Torito
# boot image is ISOLINUX chaining to MEMDISK, which needs protected mode
# this Milestone 1 real-mode-only core does not have (PC486_REVIEW.md §5.1).
# Both files come out of the one verified zip, see fetch-freedos-cd.sh.
disks/freedos-cd.iso disks/freedos-boot.img:
	bash disks/fetch-freedos-cd.sh

# --- shipped HDD image ---------------------------------------------------
# Deliberately NOT part of `check`/`test`: this runs the real FreeDOS 1.3
# installer end to end against this emulator, which takes real minutes (see
# disks/build_freedos_hdd.cpp's header and PC486_REVIEW.md §5). The tool
# boots disks/freedos-boot.img on the floppy with disks/freedos-cd.iso
# mounted on the ATAPI CD-ROM -- FreeDOS's own "boot from floppy, install
# from CD" path -- and exits non-zero without writing anything if the
# installer doesn't reach its own completion screen, so a partial image can
# never be silently accepted.
disks/build_freedos_hdd: disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ disks/build_freedos_hdd.cpp $(CHIPSET_SRCS)

hdd-image: bios disks/build_freedos_hdd disks/freedos-cd.iso disks/freedos-boot.img
	./disks/build_freedos_hdd roms/BIOS-bochs-legacy roms/VGABIOS-lgpl-latest.bin \
	  disks/freedos-boot.img disks/freedos-cd.iso disks/freedos-hdd.img

# --- boot the finished HDD image standalone (no floppy), the actual
# "does this image work?" check -- see hdd_boot_check.cpp.
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
	  srcs="$(CURDIR)/cpu80486.cpp $(CURDIR)/cpu80486.h $(CURDIR)/pic8259.cpp $(CURDIR)/pic8259.h \
	    $(CURDIR)/pit8253.cpp $(CURDIR)/pit8253.h $(CURDIR)/i8042.cpp $(CURDIR)/i8042.h \
	    $(CURDIR)/cmos_rtc.cpp $(CURDIR)/cmos_rtc.h $(CURDIR)/dma8237.cpp $(CURDIR)/dma8237.h \
	    $(CURDIR)/fdc765.cpp $(CURDIR)/fdc765.h $(CURDIR)/ega.cpp $(CURDIR)/ega.h \
	    $(CURDIR)/ega_render.cpp $(CURDIR)/ega_render.h \
	    $(CURDIR)/wd1003.cpp $(CURDIR)/wd1003.h $(CURDIR)/atapi_cdrom.cpp $(CURDIR)/atapi_cdrom.h \
	    $(CURDIR)/pcspeaker.cpp $(CURDIR)/pcspeaker.h \
	    $(CURDIR)/soundblaster.cpp $(CURDIR)/soundblaster.h \
	    $(CURDIR)/chipset.cpp $(CURDIR)/chipset.h $(CURDIR)/machine.cpp $(CURDIR)/machine.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/pc486/tests/coverage-cpp/index.html"

# --- aggregate ------------------------------------------------------
check: test pm-check vbe-check

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp bios_host pm_stub_check vbe_mode13_check disks/build_freedos_hdd disks/hdd_boot_check disks/boom_run_check render_screen
	$(MAKE) -C web clean
