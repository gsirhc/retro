# retroweb

The browser emulators and the landing page that lists them.

- [`index.html`](index.html) — the landing page (site root). Plain static HTML
  plus one image from [`assets/`](assets/) (a screenshot of the emulator's own
  rendered front panel); a `<select id="pageTheme">` that shares the
  `retro8080.theme` `localStorage` key with the emulators, so a theme choice
  carries across.
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

## The deployed site

CI builds all three front ends, fetches the pinned Altair BASIC / CP/M media,
builds the 6502 Assembler ROM from its own source, and fetches/builds the IBM
PC/AT's BIOS + shipped FreeDOS hard disk image, then **stages** the site so
URLs are clean:

```
_site/index.html      <- retroweb/index.html
_site/assets/         <- retroweb/assets/
_site/altair8800/     <- retroweb/altair8800/web/    (dev-only files stripped)
_site/assembler6502/  <- retroweb/assembler6502/web/ (dev-only files stripped)
_site/ibmpc-at/       <- retroweb/ibmpc-at/web/      (dev-only files stripped)
```

`_site/` is git-ignored, built by `make -C retroweb site` (CI runs the same
target). Preview the whole site exactly as deployed:

```sh
make -C retroweb preview       # foreground, on :8000 — dies with the terminal
make -C retroweb preview-bg    # detached: survives the terminal, gone on reboot
make -C retroweb preview-stop  # stop the detached server
```

Both build all three emulators' wasm (+ the 6502 Assembler ROM from source,
needs `cc65` — see `cpu6502/README.md`), fetch the Altair BASIC / CP/M media
and the IBM PC/AT's BIOS + shipped FreeDOS hard disk image (the latter a real
multi-minute build only the first time — see `ibmpc-at/IBM_PCAT_REVIEW.md`
§11 — instant afterward), stage `_site/`, and serve `http://0.0.0.0:8000/`
(landing page) + `/altair8800/` + `/assembler6502/` + `/ibmpc-at/` on the LAN.
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
