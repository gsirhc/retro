# Shared CPU cores

## Z80

`cpu_z80.{h,cpp}` is the Zilog Z80 used by Pac-Man, Frogger, Scramble,
Galaxian, and Galaga (three CPUs).
Instruction semantics and T-states follow the Zilog Z80 CPU User's Manual
(UM0080). It is not the Altair's 8080 — flag polarity and several opcodes
differ, so the cores stay separate.

```sh
make test-smoke   # Z80 GoogleTest only (deploy CI z80-test)
make zexdoc       # Frank Cringle's documented-opcode exerciser
make check        # Z80 + 6502 GoogleTest + zexdoc + Dormann
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

`zexdoc.com` and the Dormann `.bin` are fetched, pinned, and checksummed.
They are not committed.
