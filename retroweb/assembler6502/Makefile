# Top-level "scripts" for the 6502 Assembler emulator. Each target is a
# no-arg command:
#
#   make rom       build the real ROM from cpu6502/rom source (cc65)
#   make test      build + run the GoogleTest suite
#   make dormann   fetch + run Klaus Dormann's 6502/65C02 functional tests
#   make coverage  build + run the unit tests with llvm-cov, print a report
#   make serve     build the wasm module + serve the browser front end
#   make wasm      build the wasm module only
#   make check     rom + test + dormann (what CI would run)
#   make clean     remove all build output
#
# `make` with no target prints this list.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CPU6502_ROM_DIR = ../../cpu6502/rom

.PHONY: help rom test dormann coverage serve wasm check clean

LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- the real firmware, built from source (not fetched -- it's already
# your own source in cpu6502/rom/) ------------------------------------
rom:
	cd $(CPU6502_ROM_DIR) && bash make.sh
	@echo "  -> $(CPU6502_ROM_DIR)/tmp/firmware.bin"

# --- unit tests ---------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- Klaus Dormann's independent 6502/65C02 functional test suites -----
# (public domain test ROMs, kept out of the repo like the Altair's 8080
# diagnostics -- pinned + checksummed)
DORMANN_BASE = https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files
dormann/6502_functional_test.bin:
	mkdir -p dormann
	curl -sSfL -o $@ $(DORMANN_BASE)/6502_functional_test.bin
	echo "fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd  $@" | shasum -a 256 -c
dormann/65C02_extended_opcodes_test.bin:
	mkdir -p dormann
	curl -sSfL -o $@ $(DORMANN_BASE)/65C02_extended_opcodes_test.bin
	echo "10a2a07fa240666fa610c46accebe8d42b1000feef3aae619da15a8d152869b2  $@" | shasum -a 256 -c
dormann/dormann_host: dormann/dormann_host.cpp cpu65c02.cpp cpu65c02.h
	$(CXX) $(CXXFLAGS) -o $@ dormann/dormann_host.cpp cpu65c02.cpp

dormann: dormann/dormann_host dormann/6502_functional_test.bin dormann/65C02_extended_opcodes_test.bin
	./dormann/dormann_host dormann/6502_functional_test.bin 3469
	./dormann/dormann_host dormann/65C02_extended_opcodes_test.bin 24f1

# --- C++ coverage (source-based, llvm-cov) ------------------------------
COVFLAGS = -O0 -g -fprofile-instr-generate -fcoverage-mapping
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in cpu65c02_test via65c22_test acia65c51_test eeprom28c256_test hd44780_test bus_test machine_test assembler_test editor_test load_test; do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  objs="cpu65c02_test -object via65c22_test -object acia65c51_test -object eeprom28c256_test -object hd44780_test -object bus_test -object machine_test -object assembler_test -object editor_test -object load_test"; \
	  srcs="$(CURDIR)/cpu65c02.cpp $(CURDIR)/cpu65c02.h $(CURDIR)/via65c22.cpp $(CURDIR)/via65c22.h $(CURDIR)/acia65c51.cpp $(CURDIR)/acia65c51.h $(CURDIR)/eeprom28c256.cpp $(CURDIR)/eeprom28c256.h $(CURDIR)/hd44780.cpp $(CURDIR)/hd44780.h $(CURDIR)/bus.cpp $(CURDIR)/bus.h $(CURDIR)/machine.cpp $(CURDIR)/machine.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/assembler6502/tests/coverage-cpp/index.html"

# --- webassembly + browser front end ------------------------------------
wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

# --- aggregate ------------------------------------------------------
check: rom test dormann

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp
	rm -f dormann/dormann_host dormann/*.bin
	$(MAKE) -C web clean
