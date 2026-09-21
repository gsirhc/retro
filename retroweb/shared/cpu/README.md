# Shared Z80 core

`cpu_z80.{h,cpp}` is the Zilog Z80 used by Pac-Man, Frogger, and Scramble.
Instruction semantics and T-states follow the Zilog Z80 CPU User's Manual
(UM0080). It is not the Altair's 8080 — flag polarity and several opcodes
differ, so the cores stay separate.

```sh
make test     # named GoogleTest regressions (opcode groups, T-states, INT/NMI)
make zexdoc   # Frank Cringle's documented-opcode exerciser (the real ISA gate)
make check    # both — what CI's z80-test job runs
```

`zexdoc.com` is fetched, pinned, and checksummed (same pattern as the Altair's
8080PRE/TST8080 images and Klaus Dormann's 6502 suite). It is not committed.
A full pass is ~47 billion T-states, about 40 seconds on a laptop.

Each arcade board then only smokes that this CPU is actually wired and running
on that machine (`tests/smoke_test.cpp` plus that machine's Playwright
`smoke.spec.ts`), rather than re-testing the ISA.
