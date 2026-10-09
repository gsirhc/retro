# Shared CPU cores

## Z80

`cpu_z80.{h,cpp}` is the Zilog Z80 used by Pac-Man, Frogger, Scramble,
Galaxian, and Galaga (three CPUs).
It's the NMOS part, T-state exact: every instruction runs as its UM0080
machine cycles, so each memory and I/O strobe lands on the T-state the
real chip drives it, with the full undocumented set (MEMPTR, Q, SLL,
IXH/IXL, block-I/O flags). It is not the Altair's 8080. Flag polarity and
several opcodes differ, so the cores stay separate. Design notes and
sources are in [`Z80_REVIEW.md`](Z80_REVIEW.md).

Boards can drive the pins: `Cpu::set_int` (level) and `Cpu::set_nmi`
(edge), plus optional `Bus::wait` (/WAIT states), `Bus::tick` (T-states
as they pass) and `Bus::refresh` (/RFSH address).

```sh
make test-smoke   # Z80 GoogleTest only (deploy CI z80-test)
make zexdoc       # Frank Cringle's documented-opcode exerciser
make zexall       # Frank Cringle's full exerciser, undocumented flags included
make sst          # SingleStepTests/z80: 1,604,000 cases, bus timing per T-state
make check        # Z80 + 6502 GoogleTest + zexdoc + zexall + sst + Dormann
```

## NMOS 6502

`cpu_mos6502.{h,cpp}` is the period MOS Technology 6502 used by Asteroids
(and future Atari vector boards). It is not the W65C02S in
`assembler6502/` — NMOS quirks (JMP ($xxFF) wrap, RMW abs,X = 7, decimal
N/Z/V from the binary path, D preserved on reset/IRQ) and the full
undocumented opcode set are intentional.

```sh
make test-smoke-mos6502   # Mos6502.* GoogleTest (deploy CI mos6502-test)
make dormann              # Klaus Dormann 6502_functional_test.bin
```

`zexdoc.com`, `zexall.com`, the SingleStepTests tarball, and the Dormann
`.bin` are fetched, pinned, and checksummed.
They are not committed.
