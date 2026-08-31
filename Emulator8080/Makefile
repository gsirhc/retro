# Top-level "scripts" for the 8080 emulator. Each target is a no-arg command:
#
#   make demo    build + run the native echo demo
#   make test    build + run the GoogleTest suite
#   make cpm     build the CP/M host + run cpm/TST8080.COM if present
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

.PHONY: help demo test cpm serve wasm check clean

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

cpm: cpm/cpm_host
	@if [ -f cpm/TST8080.COM ]; then ./cpm/cpm_host cpm/TST8080.COM; \
	else echo "built cpm/cpm_host. drop a .COM in cpm/ and run:"; \
	     echo "  ./cpm/cpm_host cpm/TST8080.COM"; fi

# --- webassembly + browser front end -------------------------------
wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

# --- aggregate --------------------------------------------------
check: demo test

clean:
	rm -f emu8080 cpm/cpm_host
	rm -rf tests/build
	$(MAKE) -C web clean
