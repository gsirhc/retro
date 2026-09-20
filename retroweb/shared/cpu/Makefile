# Shared Zilog Z80 core (used by Pac-Man and Frogger).
#
#   make test    GoogleTest suite over the ISA / T-states / INT / NMI
#   make check   test (what CI runs)
#   make clean

.PHONY: help test check clean

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

test:
	cmake -S tests -B tests/build
	cmake --build tests/build -j
	ctest --test-dir tests/build --output-on-failure

check: test

clean:
	rm -rf tests/build
