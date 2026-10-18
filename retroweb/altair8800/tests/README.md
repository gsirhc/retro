# Altair 8800 board tests (GoogleTest)

Covers the 88-DCDD floppy controller and the 88-ACR cassette interface. The
8080 CPU tests live with the shared core in `retroweb/shared/cpu/tests/`.

GoogleTest is fetched automatically by CMake (`FetchContent`, needs network on
the first configure).

```sh
cd retroweb/altair8800/tests
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```
