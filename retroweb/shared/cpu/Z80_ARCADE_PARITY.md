# Z80 and the Z80 arcades -- parity backlog

The open gaps between the shared Z80 core plus the five boards built on it
(Pac-Man, Frogger, Scramble, Galaxian, Galaga) and the real 1979-81 PCBs,
plus test gaps against the repo rules in `CLAUDE.md` and `.claude/arcade.md`.
First audited 2026-10-08 against every source file. When an item is
fixed, write it up in `Z80_REVIEW.md` (Z80) or the
board's `*_REVIEW.md` (fact, why it matters, what it fixed, source) and
delete it here.

Done: the Z80 core (`Z80_REVIEW.md` §3-§9) and, on 2026-10-09, every
board item that has a source: Pac-Man P1-P5 (`PACMAN_REVIEW.md` §10),
Galaxian-family video V1-V7 and Konami sound K1-K2
(`SCRAMBLE_REVIEW.md` §10), Galaxian discrete sound G1
(`GALAXIAN_REVIEW.md` §9), Galaxian noise clock G2
(`GALAXIAN_REVIEW.md` §5), Galaga GA1-GA5 (`GALAGA_REVIEW.md` §8), and
B1-B3 with X3-X5 on all five. Closing the coverage gaps then turned up
four more, all fixed: Pac-Man's empty `$4800` range and address mirrors
(`PACMAN_REVIEW.md` §11), Galaga's MB88 instruction timing and 06XX R/W
latch (`GALAGA_REVIEW.md` §8), and the 8255's bit set/reset
(`SCRAMBLE_REVIEW.md` §10). What's left needs a schematic, a chip dump,
or Chris's call.

Rough parity today: **~97%**.

| Area | Parity | What's left |
|------|--------|-------------|
| Z80 core | 100% | none; passes zexall and all 1,604,000 SingleStepTests with bus timing |
| Pac-Man | 98% | VRAM wait states unverified; slot 0-2 offset is unconfirmed |
| Galaxian-family video (shared) | 100% | none known |
| Galaxian (board + discrete sound) | 100% | none known |
| Frogger | 99% | cocktail P2 |
| Scramble | 97% | ops $6 and $B are not in the scramble program |
| Galaga | 95% | 54XX HLE is a fixed burst |

Already in line with the rules: real 3.072 / 1.789772 MHz clocks, no
visitor turbo, high-score persistence is the sanctioned arcade exception,
every board has smoke + hiscore + DIP + keyboard specs, CI pairs exist.

# TODO

## 1. Needs a schematic or a dump

- **P6. Pac-Man VRAM wait states.** If the Midway sync-bus logic holds
  /WAIT on CPU access to video RAM during active display, the game runs
  slightly fast here. `Bus::wait` is ready. Needs the schematic.
- **K3. Scramble PAL ops `$6` and `$B`.** Stern 33A-105 and 33A-106
  do not print equations. The scramble program's checks match the
  current results. Those two ops are not in that program.
- **GA5. MB88 serial IRQ vector.** Internal SC is one shift per
  instruction cycle (Fujitsu MB8840 datasheet, Jan 1987: S1 is 1/6 of
  the clock generator, and two instruction cycles are 12 clocks). The
  vector `$06` is not in that datasheet.

## 2. Labelled departures

- **GA4. Galaga 54XX HLE.** Without `54xx.bin` every play command gives a
  fixed ~125 ms noise burst into the real DAC network. The genuine image
  drives all three DACs. A faithful HLE needs the 54XX program's sound
  tables.

## 3. Needs Chris

- **B4. Cocktail P2 controls** are unmapped on all five pages. Wiring
  them needs new visitor-facing key help text, which Chris approves first.

## Not gaps, on purpose

High-score persistence (sanctioned in `.claude/arcade.md`), self-test ROM
in place of copyrighted sets, Ms. Pac-Man aux board, `?test=1` seam.
