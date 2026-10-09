# Shared CPU cores: Zilog Z80 (Pac-Man family) and MOS NMOS 6502 (Asteroids).
#
#   make test               GoogleTest for Z80 + MOS 6502
#   make test-smoke         Z80 ISA only (deploy CI z80-test)
#   make test-smoke-mos6502 MOS 6502 ISA only (deploy CI mos6502-test)
#   make zexdoc             Frank Cringle's documented Z80 exerciser
#   make zexall             Frank Cringle's full Z80 exerciser (undocumented flags)
#   make sst                SingleStepTests/z80: every opcode, MEMPTR/Q, bus timing
#   make dormann            Klaus Dormann's 6502 functional test (NMOS)
#   make check              test + zexdoc + zexall + sst + dormann (nightly)
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: help test test-smoke test-smoke-mos6502 zexdoc zexall sst dormann check clean

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

test-smoke:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build -R '^Z80(Timing)?\.' --output-on-failure

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

ZEXALL_URL = https://raw.githubusercontent.com/anotherlin/z80emu/$(ZEXDOC_COMMIT)/testfiles/zexall.com
ZEXALL_SHA = 6e2da55147a04f28d303d5da6a1e6b771557ac244653590a0f24a2d39c8537e8

zex/zexall.com:
	mkdir -p zex
	curl -sSfL -o $@ $(ZEXALL_URL)
	echo "$(ZEXALL_SHA)  $@" | shasum -a 256 -c

zexall: zex/zex_host zex/zexall.com
	./zex/zex_host zex/zexall.com

# SingleStepTests/z80, 1000 cases per opcode with per-T-state bus traces.
# Generated from the Ares core, so a cross-check rather than silicon.
SST_COMMIT = ebe1875d48f374bcfd4b505d8eb8ee751568b5f7
SST_URL = https://codeload.github.com/SingleStepTests/z80/tar.gz/$(SST_COMMIT)
SST_SHA = f44683f0edf1c1c37f98861ccd1bfda3b9bce65b5b258d4817a0f1d2012076a7

sst/v1/00.json:
	curl -sSfL -o sst/z80-sst.tar.gz $(SST_URL)
	echo "$(SST_SHA)  sst/z80-sst.tar.gz" | shasum -a 256 -c
	tar xzf sst/z80-sst.tar.gz -C sst --strip-components=1 z80-$(SST_COMMIT)/v1
	rm -f sst/z80-sst.tar.gz

sst/sst_host: sst/sst_host.cpp cpu_z80.cpp cpu_z80.h
	$(CXX) $(CXXFLAGS) -o $@ sst/sst_host.cpp cpu_z80.cpp

sst: sst/sst_host sst/v1/00.json
	./sst/sst_host sst/v1/*.json

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

check: test zexdoc zexall sst dormann

clean:
	rm -rf tests/build zex/zex_host zex/zexdoc.com zex/zexall.com sst/sst_host sst/v1
	rm -f dormann/dormann_host dormann/6502_functional_test.bin
