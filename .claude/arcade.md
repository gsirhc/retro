# Arcade cabinets

Requirements for arcade machines under `retroweb/` (Pac-Man, Frogger,
Scramble, Galaxian, Galaga, Asteroids, Centipede, Missile Command, Space Invaders, Lunar Rescue, Battlezone, and any new cabinet). Repo-wide principles still live in
[`CLAUDE.md`](../CLAUDE.md); this file does not override them.

## High scores

Always persist the original program’s high-score **work RAM** when a user ROM
is loaded. IndexedDB key `hiscores` in that machine’s existing database, map
`programCrc → bytes`. Restore after POST has initialized the table (do not
poke during reset/POST wipe). Snapshot on RAM change from `tick()`. Skip
factory/zero bytes until restore has run. Use `arcadeHiscore` from
`shared/arcade-core.js`; the cabinet supplies only `read`/`write`, the POST
gate (`ready`), the factory check, and any odd restore/save rule as hooks.

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
| Galaga | `$8A20` (0x2D: 5 × 6-digit scores, 5 × 3 names) + `$83ED` (6, HI-SCORE strip). Factory is the table POST builds from main ROM `$02B9`/`$02BF`, not zeros |
| Asteroids | `$001D–$0030` (10 × 2-byte scores) + `$0034–$0051` (10 × 3 initials). Reset zeroes page 0, so factory is all zero. `$31–$33` are entry state, not saved |
| Centipede | EAROM `$1600–$163F` (64 B ER2055 image) |
| Missile Command | `$002C–$005B` (8 × initials + 3-byte BCD). Factory table is the ROM's own defaults, not zeros |
| Space Invaders | `$20F4–$20F5` (2 BCD HI-SCORE, low byte first). Factory is ROM `$1BF4–$1BF5` (init copies `$1B00` page to `$2000`); it shows from the next coin-up |
| Lunar Rescue | `$20CF` (10), `$20DB` (1), `$20F4–$20F5`. Factory is ten `$1B`, `$0A`, then `00 05` |
| Battlezone | `$0300–$033B` (10 × 3-byte scores, then 10 × 3 initials). Factory is what the reset loop at `$7B5C` builds: each score's first byte 5, initials from ROM `$7B7F` |

Scramble’s parent set has **numeric** scores only — no initials in the Konami
ROM. Real PCBs have no battery; persist is a labelled departure, Reset is
authentic zeros.

Playwright `web/tests/hiscore.spec.ts` on every cabinet: persist across
reload, Cancel keeps the bytes, confirm clears them. CI uses a CRC-patched
hwtest stand-in (`?test=1` `__test` seam), never a copyrighted dump. Tests
drive the real path: satisfy the POST gate the way the board would (write
the factory table, or wait for it) and click the real Reset buttons. Never
add a `__test` copy of restore or reset logic.

## Copyright

Ship a from-scratch hardware self-test ROM only. After the short diagnostic
patterns it must hold a help screen — title, `SELF TEST OK`, then the
copyright / load-your-own-set / browser-only copy — drawn with an original
font for that board (tilemap, vector strokes, etc.), not dumped game gfx.
Match the layout idea in Pac-Man's `roms/hwtest/gen_hwtest.py` `HELP_LINES`.
Copyrighted sets are opt-in, size-checked (CRC labels hwtest vs user, not a
whitelist), stored in browser IndexedDB, never fetched or uploaded.

## Fidelity and clocks

Real CPU, memory map, port polarity, and wall-clock speed on the live page.
No visitor-facing turbo. Cite period sources for quirks. Do not copy
another emulator’s shortcuts as if they were hardware.

## New cabinet checklist

- Browser WASM at the machine’s real clock.
- Front end built on `shared/arcade-core.js` (ROM loader and IndexedDB
  store, audio worklet, HIGH SCORE persister, DIP prefs, frame loop). The
  cabinet's `app.js` keeps `roleOf`/`identifySet`, `applyKeys`, and its
  HIGH SCORE address, gate and factory check.
- GoogleTest board smoke plus a help-screen assertion on the hwtest ROM.
- Playwright including `hiscore.spec.ts`, keyboard, DIP, ROM loader, home, and
  the shared specs in `retroweb/shared/arcade-tests/` (theme, seam, fullscreen,
  focus hint, footer, mute, ROM storage) via a `web/tests/cabinet.ts` fixture.
- Every wiring point in `CLAUDE.md` "Wiring a machine into the site",
  including `retroweb/Makefile` `site` / `_stage`, `retroweb/watch_site.py`
  (`make watch`), and CI `<machine>-test` / `<machine>-web-test`.
- Fixed Playwright port (do not collide with `make serve` on 8000).
- Arcade hub card in `retroweb/arcade/index.html` (marquee from
  `retroweb/shared/marquee.py`, plus `data-name` / `data-year` / `data-cpu` /
  `data-manufacturer`, `data-art` (the set's title, as named in
  libretro-thumbnails), `data-marquee` (the set name whose Arcade
  Database marquee is the cleanest scan of that cabinet's), `data-boxart-y` (the box art crop that keeps
  its logo in frame) and a year · manufacturer · CPU meta line). Cabinets
  are hub-only; the home page links in via the full-width neon
  Enter the Arcade portal on `retroweb/index.html`.
- `*_REVIEW.md` citation trail.

## Shared chips

Z80 in `retroweb/shared/cpu/`. NMOS MOS 6502 also in `retroweb/shared/cpu/`
(`cpu_mos6502`), and the Intel 8080 (`cpu_i8080`, shared with the Altair 8800). The Midway/Taito
8080 B&W board and its discrete sound board in `retroweb/shared/mw8080/`. Galaxian-family video / AY-3-8910 / i8255 in
`retroweb/shared/galaxian/` when it is the same silicon. Atari DVG, AVG, Math Box and POKEY in
`retroweb/shared/atari/`. A new related game does not fork those cores.

## Cabinet chrome

Coin door + 1P/2P start, keys legend, touch controls (`shared/arcade-touch.js`:
the cabinet's real stick restrictor and buttons, shown only on touch-only
devices, hidden after a real key press), shared theme stack, DIP panel persisted
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
