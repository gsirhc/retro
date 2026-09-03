# 8080 unit tests (GoogleTest)

Covers the arithmetic group — `ADD`, `ADC`, `SUB`, `SBB` plus the `ADI`/`ACI`/
`SUI`/`SBI` immediates — and the Zero, Sign, Parity and Carry flags they set
(Auxiliary Carry too, where the reference value is unambiguous).

GoogleTest is fetched automatically by CMake (`FetchContent`, needs network on
the first configure).

```sh
cd retroweb/altair8800/tests
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # or: ./build/arithmetic_test
```

Each test wires a `i8080::Cpu` to a 64K RAM `Bus`, assembles one instruction at
`0x0000`, runs a single `step()`, and asserts on the accumulator and flags.
Reference values come from the *Intel 8080/8085 Assembly Language Programming
Manual*.
