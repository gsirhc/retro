# Top-level "scripts" for the 8080 emulator. Each target is a no-arg command:
#
#   make demo    build + run the native echo demo
#   make test    build + run the GoogleTest suite
#   make cpm     build the CP/M host + run cpm/TST8080.COM if present
#   make cpmdisk build the 88-DCDD boot test; DISK=<img> boots a CP/M diskette
#   make serve   build the wasm module + serve the browser front end
#   make wasm    build the wasm module only
#   make test-smoke  demo + GoogleTest (what deploy CI runs)
#   make check       demo + tests + cpm (what nightly runs)
#   make coverage  build + run the unit tests with llvm-cov, print a report
#   make clean   remove all build output
#
# `make` with no target prints this list.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

SRC = main.cpp i8080.cpp serial2sio.cpp
HDR = i8080.h serial2sio.h ringbuffer.h

.PHONY: help demo test test-smoke coverage cpm cpmdisk serve wasm check clean

# llvm tools: xcrun on macOS, PATH on Linux
LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- native echo demo ---------------------------------------------------
emu8080: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

demo: emu8080
	./emu8080

# --- unit tests -------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- C++ coverage (source-based, llvm-cov) --------------------------
# Unit suites plus the 8080 diagnostic COMs through the CP/M host (covers i8080.cpp)
COVFLAGS = -O0 -g -fprofile-instr-generate -fcoverage-mapping
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	$(CXX) -std=c++17 $(COVFLAGS) -I. -o tests/build-cov/cpm_host_cov cpm/cpm_host.cpp i8080.cpp
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in arithmetic_test disk88_test cassette_test interrupt_test; do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  for c in 8080PRE TST8080 CPUTEST 8080EXM; do \
	    [ -f ../../cpm/$$c.COM ] || continue; \
	    LLVM_PROFILE_FILE=cpm_$$c.profraw ./cpm_host_cov ../../cpm/$$c.COM >/dev/null; done; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  objs="arithmetic_test -object disk88_test -object cassette_test -object interrupt_test -object cpm_host_cov"; \
	  srcs="$(CURDIR)/i8080.cpp $(CURDIR)/i8080.h $(CURDIR)/disk88.cpp $(CURDIR)/disk88.h $(CURDIR)/cassette.cpp $(CURDIR)/cassette.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/altair8800/tests/coverage-cpp/index.html"

# --- CP/M diagnostic host --------------------------------------------
cpm/cpm_host: cpm/cpm_host.cpp i8080.cpp i8080.h
	$(CXX) $(CXXFLAGS) -o $@ cpm/cpm_host.cpp i8080.cpp

# 8080PRE first (smoke), 8080EXM last (~15 s); stops at the first failure
cpm: cpm/cpm_host
	@ran=0; \
	for f in 8080PRE.COM TST8080.COM CPUTEST.COM 8080EXM.COM; do \
	  [ -f cpm/$$f ] || continue; \
	  echo "=== cpm/$$f ==="; ./cpm/cpm_host cpm/$$f || exit $$?; ran=1; \
	done; \
	[ $$ran = 1 ] || echo "cpm/cpm_host built. add a .COM and run ./cpm/cpm_host cpm/<file>"

# --- 88-DCDD disk boot test -----------------------------------------
DISK ?=
cpm/cpm_disk: cpm/cpm_disk.cpp i8080.cpp serial2sio.cpp disk88.cpp \
              i8080.h serial2sio.h ringbuffer.h disk88.h disk_bootrom.h
	$(CXX) $(CXXFLAGS) -o $@ cpm/cpm_disk.cpp i8080.cpp serial2sio.cpp disk88.cpp

# DISK=path/to/CPM.dsk boots it; with none, falls back to cpm/*.dsk, else reports success
cpmdisk: cpm/cpm_disk
	@if [ -n "$(DISK)" ]; then \
	  echo "=== boot $(DISK) ==="; ./cpm/cpm_disk "$(DISK)" DIR STAT || exit $$?; \
	else \
	  img=$$(ls cpm/*.dsk cpm/*.DSK 2>/dev/null | head -1); \
	  if [ -n "$$img" ]; then \
	    echo "=== boot $$img ==="; ./cpm/cpm_disk "$$img" DIR STAT || exit $$?; \
	  else \
	    echo "cpm/cpm_disk built. run: make cpmdisk DISK=path/to/CPM.dsk"; \
	  fi; \
	fi

# --- webassembly + browser front end -------------------------------
wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

# --- aggregate --------------------------------------------------
# includes cpm so ALU/flag/cycle regressions fail locally; no-ops if cpm/*.COM aren't fetched
test-smoke: demo test

check: demo test cpm

clean:
	rm -f emu8080 cpm/cpm_host cpm/cpm_disk
	rm -rf tests/build tests/build-cov tests/coverage-cpp
	$(MAKE) -C web clean
