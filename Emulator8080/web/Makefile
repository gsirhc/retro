# Build the WebAssembly module. Needs the Emscripten SDK on PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

# Use the C++ driver (em++), not emcc — embind pulls in libc++abi typeinfo
# that the C driver won't link.
EMCC ?= em++

SRC = wasm_machine.cpp ../i8080.cpp ../serial2sio.cpp
HDR = ../i8080.h ../serial2sio.h ../ringbuffer.h

EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=Retro8080 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0

retro8080.js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) $(SRC) -o $@

.PHONY: roms serve clean
roms: roms/echo.bin
roms/echo.bin roms/hello.bin: roms/gen_roms.py
	python3 roms/gen_roms.py

serve: retro8080.js roms
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 -m http.server 8000

clean:
	rm -f retro8080.js retro8080.wasm roms/echo.bin roms/hello.bin
