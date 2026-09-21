# Konami Frogger (1981) arcade board emulator.
#
#   make test    GoogleTest suite (board + video + AY + 8255 + board smoke).
#                Optional playthrough skips unless roms/user/ holds a dump:
#                `UserRomInsertsCoinStartsAndHops` (FROGGER_ROM / frogger.zip).
#   make check   test (what CI runs)
#   make wasm    WebAssembly module
#   make serve   wasm + test ROM + shared chrome, serve on :8000
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: help test check wasm serve clean roms

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

roms:
	$(MAKE) -C web roms

wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

check: test

clean:
	rm -rf tests/build
	$(MAKE) -C web clean
