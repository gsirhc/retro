# Shared CPU cores: Zilog Z80 (Pac-Man family) and MOS NMOS 6502 (Asteroids).
#
#   make test               GoogleTest for Z80 + MOS 6502
#   make test-smoke         Z80 ISA only (deploy CI z80-test)
#   make test-smoke-mos6502 MOS 6502 ISA only (deploy CI mos6502-test)
#   make zexdoc             Frank Cringle's documented Z80 exerciser
#   make dormann            Klaus Dormann's 6502 functional test (NMOS)
#   make check              test + zexdoc + dormann (nightly)
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: help test test-smoke test-smoke-mos6502 zexdoc dormann check clean

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

test-smoke:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build -R '^Z80\.' --output-on-failure

test-smoke-mos6502:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build -R '^Mos6502\.' --output-on-failure

# Frank Cringle's zexdoc, documented Z80 opcodes only. Pinned + checksummed.
ZEXDOC_COMMIT = 1c418fa0d719abab9273131113defbe276101d95
ZEXDOC_URL = https://raw.githubusercontent.com/anotherlin/z80emu/$(ZEXDOC_COMMIT)/testfiles/zexdoc.com
ZEXDOC_SHA = 34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f

zex/zexdoc.com:
	mkdir -p zex
	curl -sSfL -o $@ $(ZEXDOC_URL)
	echo "$(ZEXDOC_SHA)  $@" | shasum -a 256 -c

zex/zex_host: zex/zex_host.cpp cpu_z80.cpp cpu_z80.h
	$(CXX) $(CXXFLAGS) -o $@ zex/zex_host.cpp cpu_z80.cpp

zexdoc: zex/zex_host zex/zexdoc.com
	./zex/zex_host zex/zexdoc.com

# Klaus Dormann 6502 functional test (NMOS); success PC $3469
DORMANN_BASE = https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files
DORMANN_SHA = fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd

dormann/6502_functional_test.bin:
	mkdir -p dormann
	curl -sSfL -o $@ $(DORMANN_BASE)/6502_functional_test.bin
	echo "$(DORMANN_SHA)  $@" | shasum -a 256 -c

dormann/dormann_host: dormann/dormann_host.cpp cpu_mos6502.cpp cpu_mos6502.h
	$(CXX) $(CXXFLAGS) -o $@ dormann/dormann_host.cpp cpu_mos6502.cpp

dormann: dormann/dormann_host dormann/6502_functional_test.bin
	./dormann/dormann_host dormann/6502_functional_test.bin 3469

check: test zexdoc dormann

clean:
	rm -rf tests/build zex/zex_host zex/zexdoc.com
	rm -f dormann/dormann_host dormann/6502_functional_test.bin
