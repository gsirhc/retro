# Top-level "scripts" for the 8080 emulator. Each target is a no-arg command:
#
#   make demo    build + run the native echo demo
#   make test    build + run the GoogleTest suite
#   make cpm     build the CP/M host + run cpm/TST8080.COM if present
#   make cpmdisk build the 88-DCDD boot test; DISK=<img> boots a CP/M diskette
#   make serve   build the wasm module + serve the browser front end
#   make wasm    build the wasm module only
#   make check   demo + tests (what CI would run)
#   make clean   remove all build output
#
# `make` with no target prints this list.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

SRC = main.cpp i8080.cpp serial2sio.cpp
HDR = i8080.h serial2sio.h ringbuffer.h

.PHONY: help demo test cpm cpmdisk serve wasm check clean

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

# --- CP/M diagnostic host --------------------------------------------
cpm/cpm_host: cpm/cpm_host.cpp i8080.cpp i8080.h
	$(CXX) $(CXXFLAGS) -o $@ cpm/cpm_host.cpp i8080.cpp

# run each diagnostic image present in cpm/, 8080PRE first (smoke test) and
# 8080EXM last (exhaustive, ~15 s); stops at the first failure so CI fails loudly
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

# `make cpmdisk` builds the harness; `make cpmdisk DISK=path/to/CPM.dsk` boots it.
# With no DISK it falls back to any cpm/*.dsk present, else just reports success.
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
check: demo test

clean:
	rm -f emu8080 cpm/cpm_host cpm/cpm_disk
	rm -rf tests/build
	$(MAKE) -C web clean
