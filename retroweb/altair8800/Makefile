# Top-level "scripts" for the 8080 emulator. Each target is a no-arg command:
#
#   make demo    build + run the native echo demo
#   make test    build + run the GoogleTest suite
#   make cpmdisk build the 88-DCDD boot test; DISK=<img> boots a CP/M diskette
#   make serve   build the wasm module + serve the browser front end
#   make wasm    build the wasm module only
#   make test-smoke  demo + GoogleTest (what deploy CI runs)
#   make check       demo + tests (what nightly runs; 8080 diagnostics are shared/cpu)
#   make coverage  build + run the unit tests with llvm-cov, print a report
#   make clean   remove all build output
#
# `make` with no target prints this list.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

CPU = ../shared/cpu
SRC = main.cpp $(CPU)/cpu_i8080.cpp serial2sio.cpp
HDR = $(CPU)/cpu_i8080.h serial2sio.h ringbuffer.h

.PHONY: help demo test test-smoke coverage cpmdisk serve wasm check clean

# llvm tools: xcrun on macOS, PATH on Linux
LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# --- native echo demo ---------------------------------------------------
emu8080: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -I$(CPU) -o $@ $(SRC)

demo: emu8080
	./emu8080

# --- unit tests -------------------------------------------------------
test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# --- C++ coverage (source-based, llvm-cov) --------------------------
# The 8080 core's coverage is `make -C ../shared/cpu coverage-i8080`.
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in disk88_test cassette_test; do \
	    LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  objs="disk88_test -object cassette_test"; \
	  srcs="$(CURDIR)/disk88.cpp $(CURDIR)/disk88.h $(CURDIR)/cassette.cpp $(CURDIR)/cassette.h"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/altair8800/tests/coverage-cpp/index.html"

# --- 88-DCDD disk boot test -----------------------------------------
DISK ?=
cpm/cpm_disk: cpm/cpm_disk.cpp $(CPU)/cpu_i8080.cpp serial2sio.cpp disk88.cpp \
              $(CPU)/cpu_i8080.h serial2sio.h ringbuffer.h disk88.h disk_bootrom.h
	$(CXX) $(CXXFLAGS) -I$(CPU) -o $@ cpm/cpm_disk.cpp $(CPU)/cpu_i8080.cpp serial2sio.cpp disk88.cpp

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
test-smoke: demo test

check: demo test

clean:
	rm -f emu8080 cpm/cpm_disk
	rm -rf tests/build tests/build-cov tests/coverage-cpp
	$(MAKE) -C web clean
