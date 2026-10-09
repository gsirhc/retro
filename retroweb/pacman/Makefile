# Namco Pac-Man (1980) arcade board emulator.
#
#   make test    GoogleTest suite (video + WSG + machine + board smoke).
#                User-ROM playthroughs skip unless roms/user/ holds a dump.
#   make test-smoke  Smoke.* GoogleTest only (what deploy CI runs)
#   make check   test (what nightly runs)
#   make coverage  board-code llvm-cov report (no user-ROM playthroughs)
#   make wasm    WebAssembly module
#   make serve   wasm + test ROM + shared chrome, serve on :8000
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

# llvm tools: xcrun on macOS, PATH on Linux
LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV      ?= xcrun llvm-cov

.PHONY: help test check wasm serve clean roms test-smoke coverage

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

# Board smoke only (Smoke.*). Full GoogleTest is `make test` / `make check`.
test-smoke:
	cmake -S tests -B tests/build
	cmake --build tests/build -j --target smoke_test
	ctest --test-dir tests/build -R '^Smoke\.' --output-on-failure

# --- C++ coverage (source-based, llvm-cov) --------------------------
COV_TESTS = video_wsg_test machine_test smoke_test
COV_SRCS  = machine.cpp video.cpp ../shared/namco/wsg.cpp
coverage:
	cmake -S tests -B tests/build-cov -DCOVERAGE=ON >/dev/null
	cmake --build tests/build-cov -j >/dev/null
	@set -e; cd tests/build-cov && rm -f *.profraw cov.profdata; \
	  for t in $(COV_TESTS); do LLVM_PROFILE_FILE=$$t.profraw ./$$t >/dev/null; done; \
	  $(LLVM_PROFDATA) merge -sparse *.profraw -o cov.profdata; \
	  objs=$$(echo $(COV_TESTS) | sed 's/ / -object /g'); \
	  srcs="$(addprefix $(CURDIR)/,$(COV_SRCS))"; \
	  $(LLVM_COV) report $$objs -instr-profile=cov.profdata $$srcs; \
	  $(LLVM_COV) show $$objs -instr-profile=cov.profdata -format=html -output-dir=../coverage-cpp $$srcs >/dev/null; \
	  echo "  html: retroweb/pacman/tests/coverage-cpp/index.html"

roms:
	$(MAKE) -C web roms

wasm:
	$(MAKE) -C web

serve:
	$(MAKE) -C web serve

check: test

clean:
	rm -rf tests/build tests/build-cov tests/coverage-cpp
	$(MAKE) -C web clean
