# retroweb

The browser emulators and the landing page that lists them.

- [`index.html`](index.html) — the landing page (site root). Machines are
  grouped as 3-column tiles (computers, then **Arcade: Z-80 Powered**, then
  Homebrew). Computer cards use screenshots of this project's own rendered
  hardware; arcade cards are original 286×128 starfield titles in Courier
  New — see [`assets/README.md`](assets/README.md) and
  [`shared/marquee.py`](shared/marquee.py). A `<select id="pageTheme">`
  shares the `retro8080.theme` `localStorage` key with the emulators, so a
  theme choice carries across.
- [`altair8800/`](altair8800/) — MITS Altair 8800 (Intel 8080). C++ core +
  GoogleTest + WebAssembly front end in `altair8800/web/`. Deploys to
  `/altair8800/`. See its [README](altair8800/README.md).
- [`assembler6502/`](assembler6502/) — "6502 Assembler": a hand-built W65C02S
  single-board computer (see `cpu6502/pcb/pcb6502/FullBoard/`),
  CPU/VIA/ACIA/EEPROM core + GoogleTest + WebAssembly front end in
  `assembler6502/web/`. Boots to the genuine 1976 Apple-1 Wozmon, then a
  resident line-numbered editor + two-pass 6502/65C02 assembler running on
  the emulated CPU itself (`cpu6502/rom/editor.s`) — not a browser-side
  assembler. Deploys to `/assembler6502/`. Hardware findings and their
  netlist evidence trail live in
  [`CGOAC6502_REVIEW.md`](assembler6502/CGOAC6502_REVIEW.md).
- [`ibmpc-at/`](ibmpc-at/) — IBM PC/AT (5170-339): real-mode 80286 CPU +
  chipset + WD1003 hard disk + NEC 765 floppy controller + EGA graphics + PC
  speaker, C++ core + GoogleTest + WebAssembly front end in `ibmpc-at/web/`.
  Deploys to `/ibmpc-at/`. Ships pre-loaded with FreeDOS 1.3 on its virtual
  hard disk — boots straight to a `C:\>` prompt. Hardware findings live in
  [`IBM_PCAT_REVIEW.md`](ibmpc-at/IBM_PCAT_REVIEW.md).
- [`pacman/`](pacman/) — Namco Pac-Man (1980) arcade board: Z80 at
  3.072 MHz, real tilemap/sprite video and Namco 3-voice WSG, C++ core +
  GoogleTest + WebAssembly front end in `pacman/web/`. Deploys to
  `/pacman/`. Screen only — no disks or front panel, since an arcade board's
  "storage" is its ROM sockets. Ships a from-scratch hardware self-test ROM
  by default (no Namco code or graphics); a real Midway `pacman` ROM set can
  be loaded client-side and stays in the browser's IndexedDB, never
  fetched or committed. Hardware findings live in
  [`PACMAN_REVIEW.md`](pacman/PACMAN_REVIEW.md).
- [`frogger/`](frogger/) — Konami Frogger (1981) arcade board: dual Z80
  (3.072 MHz main, 1.79 MHz sound), Galaxian-family video, AY-3-8910,
  C++ core + GoogleTest + WebAssembly front end in `frogger/web/`. Deploys
  to `/frogger/`. Ships a from-scratch hardware self-test ROM; a real
  Konami/Sega `frogger` set is opt-in and browser-local. Hardware findings
  live in [`FROGGER_REVIEW.md`](frogger/FROGGER_REVIEW.md). The Z80 core is
  shared with Pac-Man at [`shared/cpu/`](shared/cpu/).

## Shared front-end code

[`shared/`](shared/) holds the front-end code common to every machine
page — the theme system (`theme-init.js`'s anti-FOUC bootstrap,
`theme-picker.js`'s interactive `<select>` logic), the pagebar/titlebar
chrome (`pagebar.css`), the fullscreen + "click to focus" mechanisms
(`fullscreen.css`/`.js`, `focus-hint.css`/`.js`), and the site footer
(`footer.css`/`.js`, including the EXIT sign that links to `about.html`).
The landing page and `about.html` link `shared/` directly; each machine's
own `web/Makefile` has a `shared` target that copies these files into that
machine's own `web/shared/` (git-ignored, regenerated like the wasm build
itself), because **only `web/` ever gets staged** — see "Adding a machine"
below. Each
machine's own `:root`/`[data-theme]` color-token *values* (and anything that
genuinely differs — a machine's own tuned spacing, an extra dead-hardware
control) stay local to that machine's `index.html`, layered on top of the
shared CSS via the cascade.

[`shared/cpu/`](shared/cpu/) is the C++ Z80 core used by Pac-Man and Frogger
— not front-end chrome, and not copied into `_site/shared/`.

## The deployed site

CI builds every front end, fetches the pinned Altair BASIC / CP/M media,
builds the 6502 Assembler ROM from its own source, fetches/builds the IBM
PC/AT's BIOS + shipped FreeDOS hard disk image, and generates Pac-Man's
hardware self-test ROM, then **stages** the site so URLs are clean:

```
_site/index.html      <- retroweb/index.html
_site/about.html      <- retroweb/about.html
_site/assets/         <- retroweb/assets/
_site/shared/         <- retroweb/shared/theme.css, footer.css, footer.js
_site/altair8800/     <- retroweb/altair8800/web/    (dev-only files stripped)
_site/assembler6502/  <- retroweb/assembler6502/web/ (dev-only files stripped)
_site/ibmpc-at/       <- retroweb/ibmpc-at/web/      (dev-only files stripped)
_site/pacman/         <- retroweb/pacman/web/        (dev-only files stripped)
_site/frogger/        <- retroweb/frogger/web/       (dev-only files stripped)
```

`_site/` is git-ignored, built by `make -C retroweb site` (CI runs the same
target). Preview the whole site exactly as deployed:

```sh
make -C retroweb preview       # foreground, on :8000 — dies with the terminal
make -C retroweb preview-bg    # detached: survives the terminal, gone on reboot
make -C retroweb preview-stop  # stop the detached server
```

Both build every emulator's wasm (+ the 6502 Assembler ROM from source,
needs `cc65` — see `cpu6502/README.md`; + Pac-Man's generated hardware
self-test ROM), fetch the Altair BASIC / CP/M media and the IBM PC/AT's
BIOS + shipped FreeDOS hard disk image (the latter a real multi-minute
build only the first time — see `ibmpc-at/IBM_PCAT_REVIEW.md` §11 — instant
afterward), stage `_site/`, and serve `http://0.0.0.0:8000/` (landing page)
+ `/altair8800/` + `/assembler6502/` + `/ibmpc-at/` + `/pacman/` on the LAN.
A bare `python3 -m http.server` in `retroweb/` will **not** work — each
emulator lives at `<machine>/web/` in source, only at `/<machine>/` in the
staged site.

**If the preview keeps dropping** (terminal closed, laptop slept, a crash),
install it as a launchd agent — `RunAtLoad` + `KeepAlive` bring it back:

```sh
make -C retroweb preview-install     # loads ~/Library/LaunchAgents/dev.retroweb.preview.plist
make -C retroweb site                # refresh what it serves, after editing source
make -C retroweb preview-uninstall   # remove it
```

## Adding a machine

New subdir `retroweb/<machine>/` with its own project + `web/` front end; add a
`.machine-card` to `index.html`; add a stage step to the `build` job in
`.github/workflows/deploy-emulator.yml`.
