# Shared Zilog Z80 core (used by Pac-Man, Frogger, and Scramble).
#
#   make test    GoogleTest suite over the ISA / T-states / INT / NMI
#   make zexdoc  fetch + run Frank Cringle's documented-opcode exerciser
#                (~40 s locally; the real ISA gate, like 8080EXM / Dormann)
#   make check   test + zexdoc (what CI's z80-test job runs)
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: help test zexdoc check clean

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# Frank Cringle's zexdoc — documented Z80 opcodes only (not zexall's
# undocumented flags). Pinned + checksummed, kept out of the repo like
# the Altair's 8080 diagnostics and Klaus Dormann's 6502 suite.
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

check: test zexdoc

clean:
	rm -rf tests/build zex/zex_host zex/zexdoc.com
