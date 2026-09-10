# retro — working principles

This repo holds recreations of vintage machines. The browser emulators live in
`retroweb/` (`retroweb/altair8800` — MITS Altair 8800 / Intel 8080, deployed to
`/altair8800/`; `retroweb/index.html` is the landing page); plus `cpu6502`,
`commodore64`, `cc65`, `kicad`. The whole point is **fidelity to the real
hardware**. When you work here, that comes first.

## Realism is the default, not an option

- **Match the real machine's behavior, timing, limits, prompts, error messages,
  I/O port addresses, and status-bit polarity.** If a real Altair did X, the
  emulator does X — even when X is slow, awkward, or surprising.
- When porting behavior from another implementation (SIMH, deramp.com, an
  original ROM disassembly, a period manual), **cite the source** in a comment
  and keep the quirk, including genuine hardware behavior that software
  depends on (e.g. the 8080's `ANA` instruction ORing bit 3 of both operands
  into auxiliary carry, instead of the more intuitive AND). But keep the
  distinction straight: a real chip/board quirk is worth preserving even when
  it looks like a bug; an artifact of a *previous emulator* is not itself
  period-accurate just because another emulator does it that way. SIMH's
  88-DCDD track-0 status only ever latches on and never re-tracks the head on
  a step-in — that's a SIMH implementation shortcut, not MITS hardware
  behavior (a real 88-DCDD reads track 0 from a physical opto sensor, so the
  line follows the head both ways) — which is why `disk88.cpp` deliberately
  does *not* port it. See `retroweb/altair8800/ALTAIR_REVIEW.md` §7.1.
- When you're unsure whether something is period-accurate, **look it up**
  (SIMH source, original manuals, ROM disassembly) rather than guessing or
  picking whatever's convenient.
- Historical detail is a feature: hardware names on the S-100 cards, the blank
  paper-tape leader, the 72-column BASIC input buffer, `MEMORY SIZE?`
  auto-detection landing on the real number, the ASR-33 crawling at 10 cps.
  Preserve and add these, don't smooth them away.

## Never speed these up — not even as an option, unless explicitly asked

- **CPU / bus / clock timing.** 2 MHz is 2 MHz. The 8080 runs at real T-state
  rates paced to wall-clock time. No "turbo", no cycle multipliers in normal
  operation.
- **Terminal / serial output rates.** A 110-baud Teletype prints at 10
  characters per second and *throttles the CPU* while it does. A VT100 at its
  real baud. Do not meter output faster than the selected terminal's rate.

(The existing `turbo` flag is a load-time-only concession used while injecting a
BASIC listing or auto-answering cold-start prompts; it ends on the first
keypress. Don't extend its use into normal running.)

## Overrides are allowed where realism is genuinely impractical — but:

1. the **default is always the realistic behavior**,
2. the override is **clearly labelled** as a departure, and
3. it's **opt-in** (a control the user chooses), never silent.

Current sanctioned overrides:

- **Paper-tape LOAD SPEED / cassette TAPE SPEED** (`Realistic` / `5×` / `25×` /
  `50×`). Realistic is the real rate (10 B/s paper tape, 30 B/s cassette) and is
  the default; the multipliers exist because a full ~14-minute 8K BASIC load (or
  ~10-minute Star Trek cassette) is impractical for casual use — even 50× is
  ~17 s for 8K BASIC, ~12 s for Star Trek, so nothing here is "instant". On the
  cassette deck the selector paces the whole transport — PLAY, FAST-FORWARD, REW;
  `CassetteACR::credit_` lets BASIC's CLOAD loop pull several bytes per frame so
  25×/50× really are that fast (not frame-capped at ~60 B/s). An internal `Max`
  (0 = unlimited) is **not** in the picker: it's what `?test=1` forces (below)
  and what preset **Auto-load** uses as its "just get me there" shortcut.
- **Auto-load software** checkbox (off by default) and the preset cold-start
  prompt auto-answering — labelled shortcuts, not the default.
- The transport keys (PLAY / REC gate the tape), the paper-tape reader's **START**
  button (runs the reader only — no bootstrap, no reset — for a loader you keyed
  in), and the front panel stay fully functional so the manual, authentic path is
  always available. The reader's **AUTO-LOAD** button is a labelled shortcut, not
  the default path.
- **Automated tests** (`?test=1`) may force paper-tape / cassette / floppy
  **load speed** to `Max` so a suite doesn't wait out a ~14-minute read or a
  needless ~166 ms/rev pause on every disk poll. The CPU stays at real 2 MHz
  under test — no cycle multiplier beyond the existing load-time `turbo`. A
  few tests select `Realistic` on purpose, to prove the throttle still works.
- **`?help`'s in-terminal manual** prints at a boosted rate (not the selected
  terminal's real baud) so the how-to page lands in ~8 s instead of minutes on
  an ASR-33; a keypress skips straight to the end regardless. This affects only
  emulator UI text, never machine output from a loaded program, so it's exempt
  from the terminal-metering rule above — but it's still a labelled shortcut,
  not the default reading speed.

If you're about to add a speed/convenience knob, stop and check it against the
three rules above. If it can't meet them, don't add it.

## Adding a new machine

A request to add a system to this repo means a fully emulated machine, not a
spec sheet or reference document. Every addition must:

- **Run entirely in the browser**, compiled to WASM (the `cpu6502`/Altair 8800
  pattern), using the existing `retroweb` theme system (Windows 95 / Mid-1990s
  Web / Modern / Dark Modern) and CRT/terminal conventions already in place.
- **Emulate the real CPU, memory map, and clock** at the machine's actual
  speed — no shortcuts on instruction timing, wait states, or bus contention
  (see "Never speed these up" above; the same opt-in-override rules apply).
- **Emulate the real graphics adapter**, reproducing its actual screen modes,
  timing, and artifacts — not a framebuffer approximation of what it looked
  like.
- **Emulate storage as real hardware**: floppy drives that accept swappable
  disk images (like the Altair's 88-DCDD), and a hard disk that ships
  pre-loaded with the system's original OS, exactly as the real machine left
  the factory.
- When the original ROM/OS is still under active copyright (e.g. IBM's AT
  BIOS, PC-DOS) — unlike the Altair's 1975 BASIC, which the community treats
  as freely redistributable — don't bundle or auto-fetch the genuine
  firmware. Substitute a freely-licensed, hardware-compatible replacement (an
  open BIOS implementation, FreeDOS in place of PC-DOS) so the public build
  stays turnkey like every other machine here, and label the substitution
  clearly (README, boot banner) as a compatible stand-in, not the literal
  factory firmware — the same labelled-departure rule as any override above.

## Wiring a machine into the site

`retroweb/index.html` (the landing page) and `retroweb/Makefile` (site
staging + local preview) are shared across every machine — getting a new
one to actually show up and run is a separate step from building the
machine itself (see "Adding a new machine" above).

- **Landing-page card**: add a `.machine-card` `<a>` in `retroweb/index.html`
  (screenshot in `retroweb/assets/`, name, "Launch →") — copy an existing
  one's markup exactly, just swap the `href`, image, and name.
- **Staging**: `retroweb/Makefile`'s `site` target copies each machine's
  `<machine>/web/` into `_site/<machine>/`, stripping dev-only files
  (`Makefile`, `devserve.py`, `wasm_machine.cpp`, test tooling) — add a
  `mkdir`/`cp -R`/strip block there for the new machine, and a build step
  (wasm + roms + any pinned media) in the `_stage` target above it.
- **Only `web/` ever gets staged** — the deployed site (and `make preview`,
  which serves the exact same `_site/`) has no access to anything one level
  up via `../`. Any asset a machine's page fetches at runtime (ROMs, disk
  images) must be copied into that machine's own `web/roms/`, `web/disks/`
  etc. by *that machine's* `web/Makefile` (from wherever its native-core
  fetch/build scripts already produced it, so there's one pinned/verified
  source, not a second copy) — see `retroweb/ibmpc-at/web/Makefile`'s
  `roms`/`hdd-image` targets for the pattern.
- **Local preview**: `make -C retroweb preview` (foreground) / `preview-bg`
  (detached, survives the terminal) serve `_site/` on `:8000`, bound to
  `0.0.0.0` — reachable on the LAN, not just `localhost`. Both go through
  `retroweb/serve_nocache.py`, not a bare `python3 -m http.server`: without
  explicit `Cache-Control: no-store`, a browser can keep serving a stale
  wasm module/app.js after `make site` re-stages fresh ones underneath it,
  which looks exactly like a regression that isn't actually there (a real
  bug this repo hit once already — see `retroweb/ibmpc-at/IBM_PCAT_REVIEW.md`
  §15). `preview-install` loads it as a launchd agent so it survives sleep/
  logout/reboot; `preview-stop`/`preview-status`/`preview-uninstall` manage it.
- **Each machine's own `web/Makefile`** should default its *own* `make serve`
  to port `8000` too (matching every machine — you only ever run one
  machine's dev server standalone at a time) and reserve a separate,
  *fixed* port for its own Playwright suite instead (`8100` altair8800,
  `8200` assembler6502, `8300`/`8310` ibmpc-at) specifically so a hand-run
  `make serve` and an automated test run never collide.
- Full details: `retroweb/README.md`.

## Delegate to a cheaper model when the task allows it

If you're running as Sonnet, hand well-scoped, mechanical subtasks to a
Haiku subagent instead of doing them yourself; if you're running as Opus,
delegate down to Sonnet or Haiku the same way, whichever fits. Good
candidates: running a fixed build/test command and reporting results,
fetching and checksum-verifying a pinned asset, writing a test that
mechanically follows an established pattern in the same file, a rename or
mechanical refactor with an exact, fully-specified shape. Keep for yourself
anything that leans on context you're already holding that a fresh agent
would have to re-derive (a debugging session mid-diagnosis, a decision
that needs the citation/period-accuracy discipline this repo runs on,
anything where getting it wrong is expensive to catch later) — re-deriving
that context usually costs more than the delegation saves. When genuinely
unsure whether a task is a good candidate, don't guess — do it yourself
rather than risk a wrong answer from a smaller model on something that
matters.

## Conventions

- `*.bin` and `*.dsk` are git-ignored — binary media (BASIC images, disk images)
  are fetched at build time by the `fetch-*.sh` scripts, not committed.
- Commit messages: no AI attribution, no `Co-Authored-By` trailer.

## Build / test

- **Altair 8800** (`retroweb/altair8800/`): `make -C retroweb/altair8800 test`
  (GoogleTest), `make -C retroweb/altair8800/web` (wasm via emscripten).
- Front-end lives in `retroweb/altair8800/web/`; `retro8080.js`/`.wasm` and
  `roms/*.bin` (the generated ones) are regenerated by CI, not committed.
- **Web integration suite:** `make -C retroweb/altair8800/web test` (Playwright;
  needs Node + `npx playwright install chromium`). Drives the real UI in a
  browser and asserts control behaviour + terminal output. `home.spec.ts` covers
  the `retroweb/index.html` landing page (shared `retro8080.theme`).
- **Coverage:** `make -C retroweb/altair8800/web coverage` (app.js, V8/monocart)
  and `make -C retroweb/altair8800 coverage` (i8080/disk88/cassette, llvm-cov).
  Neither gates CI. Use `/* v8 ignore next */` only for genuinely unreachable
  defensive paths, never to hide an untested feature.
- **Every control, device, terminal behaviour, and preset has an automated
  test.** `retroweb/altair8800/web/tests/` (Playwright) covers the front end;
  `retroweb/altair8800/tests/` (GoogleTest) covers the core. A change that adds
  or alters behaviour adds or updates a test in the same commit. CI runs both
  suites and **does not deploy a red build**.
- The **deployed site** is staged in CI: `retroweb/index.html` → `_site/`, the
  built `retroweb/altair8800/web/` → `_site/altair8800/`. Preview locally the
  same way (see `retroweb/README.md`).
