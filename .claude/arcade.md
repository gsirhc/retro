# Arcade cabinets

Requirements for arcade machines under `retroweb/` (Pac-Man, Frogger,
Scramble, Galaxian, and any new cabinet). Repo-wide principles still live in
[`CLAUDE.md`](../CLAUDE.md); this file does not override them.

## High scores

Always persist the original program’s high-score **work RAM** when a user ROM
is loaded. IndexedDB key `hiscores` in that machine’s existing database, map
`programCrc → bytes`. Restore after POST has initialized the table (do not
poke during reset/POST wipe). Snapshot on RAM change from `tick()`. Skip
factory/zero bytes until restore has run.

**Reset HIGH SCORE** (`#resetHiscore`) is the labelled way back to a
power-cycle empty table: a `.site-dialog` confirm (Cancel / Reset, not
`window.confirm`) deletes that CRC’s save and `machine.reset()`s the board.
The button is disabled on the self-test ROM.

Do **not** invent an initials overlay or map letter keys. Only persist bytes
the ROM actually keeps:

| Cabinet | RAM |
|---|---|
| Pac-Man | `$4E88–$4E8A` (3 BCD TOP) |
| Galaxian | `$40A8` (3 BCD HI-SCORE) |
| Frogger | `$83EF–$83FA` (display HI + 5-rank table) |
| Scramble | `$4200` (10 × 3 BCD) + `$40A8` (displayed HI) |

Scramble’s parent set has **numeric** scores only — no initials in the Konami
ROM. Real PCBs have no battery; persist is a labelled departure, Reset is
authentic zeros.

Playwright `web/tests/hiscore.spec.ts` on every cabinet: persist across
reload, Cancel keeps the bytes, confirm clears them. CI uses a CRC-patched
hwtest stand-in (`?test=1` `__test` seam), never a copyrighted dump.

## Copyright

Ship a from-scratch hardware self-test ROM only. Copyrighted sets are
opt-in, size-checked (CRC labels hwtest vs user, not a whitelist), stored
in browser IndexedDB, never fetched or uploaded.

## Fidelity and clocks

Real CPU, memory map, port polarity, and wall-clock speed on the live page.
No visitor-facing turbo. Cite period sources for quirks. Do not copy
another emulator’s shortcuts as if they were hardware.

## New cabinet checklist

- Browser WASM at the machine’s real clock.
- GoogleTest board smoke.
- Playwright including `hiscore.spec.ts`, keyboard, DIP, ROM loader, footer/home.
- `retroweb/Makefile` `site` / `_stage`; CI `<machine>-test` and `<machine>-web-test`.
- Fixed Playwright port (do not collide with `make serve` on 8000).
- Landing-page card (arcade tiles from `retroweb/shared/marquee.py`).
- `*_REVIEW.md` citation trail.

## Shared chips

Z80 in `retroweb/shared/cpu/`. Galaxian-family video / AY-3-8910 / i8255 in
`retroweb/shared/galaxian/` when it is the same silicon. A new related game
does not fork those cores.

## Cabinet chrome

Coin door + 1P/2P start, keys legend, shared theme stack, DIP panel persisted
in `localStorage` (physical switches survive a power cycle). Cocktail P2
stick is unmapped and labelled. Only `web/` is staged: copy `retroweb/shared/`
into each machine’s `web/shared/`, never `../` at runtime.

## Inputs

`window` keydown/keyup → cabinet port bits. Every control, device, and DIP
has an automated test.

## Overrides

Same three rules as `CLAUDE.md` (realistic default, labelled, opt-in). High-score
persist is the sanctioned arcade exception: always on for user ROMs, labelled
Reset to clear.
