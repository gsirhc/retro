# Provenance

Everything in this repo is meant to be built from primary sources:
schematics, datasheets, period manuals, and disassembly of the original
ROMs. For a while it wasn't.

## What happened

Much of the emulator code here was written with AI coding assistants. In
October 2026 I found that some of that code had been translated or copied
from other emulators, and that comments and review docs cited those
emulators as sources. I hadn't asked for that and didn't know it was
happening. It went against what this project is about, and in some cases
against those projects' licences, since this repo is MIT.

The affected code came from these projects, under their own licences:

- [MAME](https://github.com/mamedev/mame): arcade sound circuits, video
  decode, custom chip behaviour, the MB88 CPU core, high-score tables,
  the Ms. Pac-Man aux board tables, the Centipede IRQ and EAROM logic, and
  the Space Invaders gel artwork.
- [SIMH](https://github.com/open-simh/simh): the Altair 88-DCDD disk
  controller and its boot ROM bytes.
- [DOSBox-X](https://github.com/joncampbell123/dosbox-x): Sound Blaster
  ADPCM playback.
- [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3) and
  [ymfm](https://github.com/aaronsgiles/ymfm): parts of the OPL3 envelope,
  vibrato and key-scale tables.
- [Bochs](https://github.com/bochs-emu/Bochs): VBE bank and mode-set rules.
- [86Box](https://github.com/86Box/86Box): an EGA graphics blink rule.
- [FBNeo](https://github.com/finalburnneo/FBNeo): the Galaga 51XX fallback
  closely followed FBNeo's.

The arcade self-test screens also used a font that was mostly the
Commodore 64 character ROM while calling itself original.

Commits before `db279cc` can contain that code. It was added between
September and October 2026.

## What I did about it

- 18 October 2026: added a rule to `CLAUDE.md` that no emulator code may be
  copied, ported or translated (commit `2007eb2`), and later a lint check
  that fails the build on emulator citations (`417925a`).
- 18 to 21 October: rewrote each affected piece from primary sources, or
  removed it where no primary source exists (Sound Blaster ADPCM). The
  commits from `1ebcc18` to `a1d039c` each say what was replaced and what
  it is now based on.
- 21 to 22 October: two more audits compared the whole tree against those
  emulators' source, by reading and with automated table and code
  similarity scans. They found more, which was rebuilt from schematics,
  datasheets and the ROMs (`3bbcb11` to `db279cc`). The self-test font was
  replaced with one drawn by a stroke rule and checked glyph by glyph
  against the Commodore and IBM fonts. The Ms. Pac-Man aux board tables
  can now be re-derived from the ROM dumps with `pacman/tools/derive_aux.py`.

Where no primary source exists for a single value, the value is kept and
listed as Unsourced in that machine's `*_REVIEW.md`. Code, tables and
curves never come from another emulator.

## Today

As of commit `db279cc`, I know of no code in this tree that was taken from
another emulator. If you find something, please open an issue.

The Bochs BIOS and the LGPL VGA BIOS are fetched at build time as guest
firmware for the PC machines. They run inside the emulator as substitutes
for IBM's BIOS and are used under their own licences. The PC text fonts are
IBM's VGA ROM font, fetched at build time from Yossi Gil's fntcol16
collection.
