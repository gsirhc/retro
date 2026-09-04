# retro 8080 — browser front end

WebAssembly build of the 8080 core + Altair 88-2SIO board, wired to an
[xterm.js](https://xtermjs.org/) terminal.

`vendor/` holds xterm 5.3.0 + the fit addon and two bitmap fonts (VT323,
Courier Prime — both OFL), checked in so the page needs no network at runtime.
Refresh xterm with:

```sh
curl -Lo vendor/xterm.js            https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.js
curl -Lo vendor/xterm.css           https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.css
curl -Lo vendor/xterm-addon-fit.js  https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.js
```

```
xterm keystrokes ─► Machine.sendByte()      → 2SIO channel A RX ring buffer
Machine.readOutput() ─► xterm.write()        ← 2SIO channel A TX ring buffer
```

## Build

Needs the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH` (`source ./emsdk_env.sh`, or `brew install emscripten`). The Makefile
links with `em++` — the C driver (`emcc`) won't pull in the libc++abi typeinfo
that embind needs.

```sh
make          # -> retro8080.js + retro8080.wasm
make serve    # build, then python3 -m http.server 8000
```

Open <http://localhost:8000>. (Serve over HTTP — browsers won't fetch `.wasm`
from `file://`.)

## Tests

`tests/` is a [Playwright](https://playwright.dev) suite (~213 tests) that drives
the real page in a headless browser and asserts control behaviour, machine
state, and xterm buffer contents — the front panel, every terminal profile,
every era preset, the paper-tape reader, the 88-ACR cassette, both 88-DCDD
drives, the panel bootstrap guide, every dialog and error path, and the timing
guarantees (2 MHz CPU, 10 cps Teletype, throttled paper-tape reads). No
screenshot comparison. `workflow.spec.ts` runs each preset end to end the way a
knowledgeable user would — load Kill the Bit and watch the bit march; cold-start
4K BASIC and run a program; 8K BASIC off paper tape then `CLOAD` Star Trek and
`RUN` it; boot CP/M, `STAT` / `DIR` the disk, run `MBASIC`. `panel-lights.spec.ts`
covers every lamp: the 16-bit address decode (incl. per-bit brightness), the
data lamps, `WAIT` / `HLTA` / `INTE` / `MEMR` / `M1` / `INP` / `WO` / `OUT`,
and Kill the Bit's sense-switch XOR.
`home.spec.ts` covers the `retroweb/` landing page (served by a 2nd `webServer`
on `:8110`): its theme selector shares the `retro8080.theme` key, and the Altair
card links to `/altair8800/`. `app.js` is at 100% line and 100% function coverage
(lcov), ~82% branch; the handful of never-taken branches are `x || fallback`
short-circuits and `?.` guards. A few genuinely unreachable spots (missing
xterm, wasm-instantiate failure, self-restoring flash timers) are marked
`/* v8 ignore */`.

```sh
make test-install    # once: npm ci + a headless Chromium
make test            # build + fetch media + run the suite
make coverage        # same, one worker, V8 coverage for app.js -> coverage/index.html
npx playwright test cassette --headed      # watch one spec
npx playwright show-report                 # last run's HTML report
```

`make coverage` reports line / branch / function coverage of `app.js`
(`monocart-coverage-reports`, opt-in via `COVERAGE=1`) and writes
`coverage/index.html`. It does not gate CI. `/* v8 ignore next */` marks the
few genuinely unreachable defensive paths (missing xterm, wasm-instantiate
failure, oversized-manual reflow) — not untested features.

For the C++ core: `make -C retroweb/altair8800 coverage` (llvm-cov over
`i8080.cpp` / `disk88.cpp` / `cassette.cpp`, driven by the GoogleTest suites
plus the 8080 diagnostic COMs).

The page exposes an inspection seam under `?test=1` (`window.__test`): it hands
tests the `machine` / device objects and panel state, and forces paper-tape /
cassette / floppy **load speed** to `Max` so a run isn't held up by a
14-minute read. The CPU still runs at a real 2 MHz — `fidelity.spec.ts` checks
that. See `../../CLAUDE.md`.

CI (`deploy-emulator.yml`) runs this suite in a `web-test` job; the Pages deploy
is gated on it.

## Files

| File | Role |
|---|---|
| `wasm_machine.cpp` | embind wrapper: `Machine` = sized RAM + `i8080::Cpu` + `Serial2SIO` + `Disk88` + `CassetteACR` |
| `index.html` | page shell + the paper-tape / cassette / diskette dialogs; loads xterm + the wasm module |
| `app.js` | terminal wiring, the per-frame `runCycles` loop, program loader, front panel, 88-DCDD cabinet, register readout |
| `roms/` | built-in programs + `manifest.json` catalog the loader fetches (see `roms/README.md`) |
| `disks/` | 8-inch floppy images + `manifest.json` for the 88-DCDD cabinet (see `disks/README.md`) |
| `tests/` | Playwright integration + fidelity suite; `playwright.config.ts` serves the page on port 8100 |

The footer's "Last built" date comes from a `HEAD` request for
`retro8080.wasm` — its `Last-Modified` header, i.e. the last `make wasm`.

## Runtime model

`app.js` calls `machine.runCycles(2_000_000 / 60)` once per `requestAnimationFrame`,
then drains `machine.readOutput()` to the terminal. The 2SIO ring buffers
(512 bytes each direction) absorb the mismatch between the emulated 2 MHz clock
and the browser's frame cadence, so no bytes are lost between frames.

## Page theme

**Theme:** in the top bar switches the page chrome (not the terminal):

- **Windows 95** (default) — teal desktop, gradient navy title bar, beveled gray
  window; the title bar carries the theme picker and its close box goes home.
- **Mid-1990s Web** — NCSA Mosaic / early-Netscape page chrome: a tiled "blue
  marble" desktop (a ~4&nbsp;KB inline JPEG, baked from SVG turbulence so it
  renders the same everywhere) with a gray `border: ridge` content column on top,
  centred Times headings, an engraved `<hr size="2">` shaded rule, raw underlined
  links (`#00f` / `#800080` visited / `#f00` active), and a period footer
  (plain-text "Page last modified", a `mailto:`, pipe-separated links). Chrome
  only — the emulated hardware stays photoreal, like every theme.
- **Modern** — clean white card, system font, no window chrome. The page title
  moves into the top bar and the big in-page header / spec line are dropped, to
  save vertical space (the retro themes keep the centred `ALTAIR 8800` + `~ * ~`
  banner). On a screen &ge; 1560&nbsp;px wide it goes two-column (terminal left,
  front panel right) with the paper-tape / disk / cassette devices in a row
  underneath, and the **TERMINAL** settings fold into the **PRESET** bar. The
  terminal column keeps a &ge; 680&nbsp;px min so it never squishes; the panel
  column yields (it scrolls internally) when both don't fit. Narrower, it's a
  single column like the retro themes.
- **Dark Modern** — the Modern theme's exact layout with a dark palette. Stored
  as `moderndark`; the theme scripts set `data-theme="modern"` +
  `data-mode="dark"`, so every Modern layout rule still applies and only a
  `[data-mode="dark"]` colour-var block is added.

Every theme flows **front panel &rarr; terminal &rarr; devices** now (the reader,
disk and cassette used to sit above the terminal).

Mid-1990s Web and Modern have no window buttons, so their top bar shows an
"&larr; All machines" link back to the landing page instead.

Themes are CSS custom properties keyed off `:root[data-theme]` (plus
`[data-mode="dark"]` for Dark Modern); an inline `<head>` script applies the
stored choice before first paint (no flash). `?theme=web94` / `?theme=moderndark`
override.

## Terminal profiles

The **Terminal:** dropdown swaps the xterm face, phosphor colour, CRT overlay
(scanlines / paper texture / glow / flicker), cursor, CAPS default, and **baud
rate**. Choices: Modern (instant), DEC VT100 green/amber (9600), DEC VT52
(4800, heavy scanlines), Lear Siegler ADM-3A (9600, underline cursor), Glass TTY
(300), Teletype ASR-33 (110, uppercase, cream paper, mechanical bell).

Baud is real: output is metered onto the screen at `baud/10` chars/s, and
because the 2SIO's 512-byte FIFO fills and drops TDRE, an output-bound program
(BASIC `LIST`) actually *waits* for the terminal — a 110-baud Teletype throttles
the 8080 exactly as it did in 1975. Choice persists in `localStorage`;
`?term=vt52` overrides.

Escape handling is real too, not just cosmetics: xterm.js is always a full
VT100+ parser, but a real period terminal wasn't, so each profile's bytes run
through a small stateful filter (`TERM_PROFILES[key].filter()`) before xterm
ever sees them:
- **ASR-33 / Glass TTY** have no video memory to address — no cursor moves, no
  clear screen, no reverse video. Only CR/LF/BS/BEL/HT are real; every escape
  sequence is swallowed. (Backspace needs no special handling: xterm's default
  BS already just moves the cursor left without erasing, the same as a print
  head backing up to overstrike.) The ASR-33 is also mechanically uppercase
  only, so CAPS LOCK is forced on and the checkbox disabled on that profile.
- **VT52** translates the real DEC VT52 escape set (`ESC A/B/C/D` cursor,
  `ESC H` home, `ESC J`/`K` erase, `ESC Y row col` direct addressing, `ESC I`
  reverse index) into xterm-compatible sequences; keypad-mode and identify
  codes have no xterm equivalent and are dropped.
- **ADM-3A** translates its bare control-code cursor moves (`^K` up, `^L`
  right, `^Z` clear+home; `^H`/`^J` already match BS/LF) and `ESC = row col`
  direct addressing — the exact codes WordStar's ADM-3A driver emits.
- **VT100 green/amber** and **Modern** have no filter: they're genuinely
  ANSI/xterm-compatible, so bytes pass straight through.

See `ALTAIR_REVIEW.md` §4.

## Front panel

`app.js` builds an Altair-8800 panel above the terminal, driven by the wasm
`Machine`:

- **Address LEDs** — PC when stopped; while the CPU runs they show per-bit
  *brightness*: `Machine::busActivityCounts()` counts how many times each
  address bit was touched since the last frame, the way a real incandescent
  lamp integrates brightness over many bus cycles. A bit driven almost every
  cycle reads brighter than one touched once in passing — which is the whole
  visual effect in **Kill the Bit**, where the target bit (four `LDAX D` per
  loop) needs to read brighter than whatever the delay counter's fetch
  addresses happen to touch. **Data LEDs** = the byte at the last real bus
  address. Status LEDs from `state()`, refreshed every frame: `WAIT`/`HLTA`/
  `INTE` are exact CPU state; `MEMR`/`WO`/`INP`/`OUT`/`INT` reflect the real
  last bus access (which Bus callback fired last, tracked in
  `wasm_machine.cpp` — a write really drops `WO`, an `IN` really lights
  `INP`, not "the CPU happens to be running"). `M1` and `STACK` stay
  approximated: telling an opcode fetch apart from a plain operand read, or a
  stack push/pop apart from any other memory access, needs the CPU core
  itself to say what an access is *for*, not just which callback fired — and
  even if it did, both are sub-frame pulses a once-a-frame poll can't
  usefully observe anyway. `HLDA`/`PROT` are correctly always off (no
  DMA-capable peripheral ever asserts HOLD; PROTECT is a documented no-op).
  See `ALTAIR_REVIEW.md` §3.6a.
- **A0–A15 toggles** — the top 8 (`A8`–`A15`) feed `setSenseSwitches()` live.
- Bat-handle paddles: click the **top half** for a switch's upper function,
  the **bottom half** for the lower one. **STOP/RUN** pauses the frame loop;
  **SINGLE STEP** runs one instruction (`stepOne`) while stopped;
  **EXAMINE / EXAMINE NEXT** set PC from the switches / PC+1;
  **DEPOSIT / DEPOSIT NEXT** poke `writeByte`; **RESET/CLR** = `reboot` + PC 0.
- **OFF/ON** kills the machine and blanks every LED.
- `PROTECT`, `AUX` paddles are cosmetic.

**"Load it yourself"** — a toggle button in the **PRESET** bar that opens a
**movable floating panel** (`buildPanelGuide`) on the right, so the front panel
and the guide are both visible without scrolling; drag its title bar to
reposition (the spot is remembered), × to close. It shows how a hand-load worked, for the *current* machine: the disk boot
(`EXAMINE 0FF00, RUN`) and the serial bootstrap keyed in byte by byte. Every
value is drawn as a **row of little front-panel toggles** (grouped in 3s like
the real panel, knob up = 1) with the octal beside it, so you copy the picture,
not the number.

It doubles as a **checklist**. Three small buttons sit beside each graphic:
**↕** sets the panel's switches to that value, **▸** sets them *and* flips the
step's paddle (EXAMINE for an address, DEPOSIT for a data byte, advancing the
address), **✓** ticks the step off. Clicking any of them greys the row and locks
its ↕/▸ (✓ stays, to undo). So you can click through all 40 bytes and watch the
switches and data lamps move, tick off the ones you keyed on the real panel by
hand, or hit **Key the loader in for me** to drop all 40 and tick the whole
list. **Feed tape & run** points PC at the loader's start and spools the
threaded tape into the 88-2SIO with *no* bootstrap of its own
(`paperTape.feedRaw` → `tape.run({raw:true})`) so the hand-keyed loader catches
it — at the reader's LOAD SPEED, so "Realistic" really is a ~14-minute 8K BASIC
load with the address lamps climbing. (The reader's own **START** button is the
same raw path, minus the "point PC at the loader" convenience.) A hand-keyed loader answers none of
BASIC's cold-start prompts — you do that yourself, as on real hardware. The
listing shows all 40 rows inline (the panel scrolls as one). The checklist
resets when the machine/loader changes; re-renders when the preset changes. The
loader bakes in the tape's byte count, so threading a different tape after you've
keyed it in silently re-keys it to match (`buildPanelGuide`) — the "keyin →
thread → Feed" order works either way round.

## Era presets

The **PRESET** dropdown at the top configures a period-correct machine in one
click — RAM size, primary terminal, which S-100 cards are plugged in (shown in
the backplane strip at the very bottom of the page), and **which loader devices
are fitted**
(paper-tape reader / 88-ACR cassette / 88-DCDD floppy), each with its media
already threaded. Tick **Auto-load software** (off by default, remembered
between sessions) and the preset also loads that media:

| id | Preset | RAM | Terminal | Loader → software |
|---|---|---|---|---|
| `baremetal` | Bare-Metal Toggle (1976) | 4 KB | ASR-33 | paper tape → Kill the Bit |
| `stock` | Stock Launch (1976) | 4 KB | ASR-33 | paper tape → Altair 4K BASIC |
| `cassette` | Cassette Hobbyist (1976) | 32 KB | ADM-3A | paper tape → 8K BASIC, cassette → Star Trek |
| `cpm` | CP/M Workstation (1979) | 64 KB | VT100 | floppy → CP/M 2.2 |

`baremetal`/`stock` are dated 1976, not the earlier date their software's real
MITS release would suggest: every preset's console is the 88-2SIO (the only
serial board this emulator implements), and a genuinely period 1975 machine
would have shipped the earlier 88-SIO at ports 0x00/0x01 instead. See
`ALTAIR_REVIEW.md` §5.2 for the fuller options; this is the deliberate,
documented "relabel" choice, not an oversight.

RAM is contiguous from 0 (`machine.setRam(kb)`); above it the bus floats high,
so BASIC's `MEMORY SIZE?` auto-detect lands on the real number and a 4 KB
machine can't run an 8 KB program. A ROM image loaded above the ceiling (the
0xE000 BASIC build, the disk PROM) stays readable.

With **Auto-load** off you get the hardware with the media threaded but not
read; ticking it (or AUTO-LOAD / BOOT / `CLOAD` on the device) loads it. The
**— custom —** build shows a **Load devices** row of checkboxes — add or remove
the reader, deck, and drive to taste; the choice persists in `localStorage`.
Changing the terminal by hand flips a named preset to custom. The preset choice
persists; `?preset=cpm` in the URL wins over the stored one.

The **ⓘ Guide** button next to the dropdown opens a dialog: what the current
setup is and how to drive it (its software, the key commands), plus a short
primer on the panel and terminals. `?help` still prints the old scrolling
how-to to the terminal.

Presets 4 (Cromemco Dazzler) and 5 (Processor Tech VDM-1) are not built yet.

## Paper-tape reader

The reader shows a punched strip running past a read head. Click it to open
**Load Paper Tape** — the machine-code entries from `roms/manifest.json` (each
probed for availability) plus a thread-your-own-`.bin` picker with editable load
address / start PC. **Thread** a tape, pick a **LOAD SPEED** (Realistic / 5× /
25× / 50×, remembered in `localStorage`).

The reader has two buttons, matching the fact that on real iron the reader and
the CPU are independent:

- **START** — just runs the reader motor. It spools the tape into the 88-2SIO
  and nothing else: no memory touched, no bootstrap, no RUN
  (`paperTape.startReader` → `tape.run({raw:true})`). A loader has to already be
  running — the boot PROM, or ~20 bytes keyed in at the front panel (see the
  **"Load it yourself"** guide, whose *Feed tape & run* is the same path). It
  honours LOAD SPEED like any read. If nothing is draining the 88-2SIO the
  reader overruns it — the tape still spools to the end at the selected rate and
  the bytes are simply lost, exactly like the reader lever on an ASR-33. START
  toggles to **STOP** while the tape moves, so you can halt it mid-tape.
- **AUTO-LOAD** — the labelled shortcut (like the preset **Auto-load**
  checkbox). It resets the machine, keys `makeBootstrap()` — a ~40-byte serial
  loader — into the top page of RAM (`ramTop − 0x40`), runs it, reads the tape,
  and answers BASIC's cold-start prompts for you.

`makeBootstrap()` skips blank leader, then stores the image and `JMP`s to it. The CPU runs it while `tape.tick()` feeds the 88-2SIO:
**128 blank leader frames** (the tape physically spooling, address lamps frozen)
then the image, the write pointer climbing on `A0…A15`. **Realistic = 10 B/s**,
an ASR-33 Teletype reader — 8K BASIC genuinely takes ~14 minutes, as it did off
a MITS tape; **5× / 25× / 50×** (500 B/s → still ~17 s for 8K BASIC). When the
image ends and the FIFO drains, the bootstrap jumps to the program; for
`basic8k` / `basic4k` tapes the cold-start prompts (`MEMORY SIZE?` …) are then
answered for you. A machine without room for `image + bootstrap` refuses with a
note. (The bootstrap assumes the image's first byte is non-zero — true of every
MITS load-and-go image.)

**LOAD SPEED changes live.** Started on Realistic and don't want to wait out 8K
BASIC? Bump the selector to 50× mid-read and the feed rate changes on the
next frame — `tape.tick()` accrues a byte budget rather than replaying from
`t0`, so there's no jump. The reader's hint line shows `reading tape — NN%`
while it runs. (The cassette's TAPE SPEED is live too — it feeds `cycles_per_byte_`
straight to `CassetteACR`.)

`tape.tick()` normally holds the feed back when the 88-2SIO's 512-byte FIFO is
near full, so a per-frame CPU burst never misses a byte a real
continuously-clocked loader would have caught. But if the FIFO stays jammed for
~0.75 s on a raw START feed (`this.flooding`), the consumer clearly isn't
keeping up — or isn't there — and the reader stops holding back: the tape runs
to the end at its selected rate and the 2SIO overruns, dropping bytes, exactly
as the hardware would. The bootstrap (`AUTO-LOAD`) path always keeps up, so it
never trips.

The one exception is the **`basicRom`** entry (the 0xE000 EPROM build) — it sits
above RAM and can't stream, so it drops straight in. Preset **Auto-load** uses an
internal unlimited speed (not in the picker); press **AUTO-LOAD** (or START, with
a loader running) yourself to watch a tape read at the selected speed.

The front-panel **RESET/CLR** paddle is the machine's reset (PC → 0, RAM
intact) — a bus signal to the CPU and the S-100 cards' electrical state, not a
mechanical rewind. The 88-DCDD controller *is* on the bus, so it deselects and
its latches clear, but its heads don't move (RESET carries no STEP pulses —
see `disk88.cpp`'s `reset()`). The 88-ACR cassette deck isn't on the bus at
all — it's a separate box on its own cable — so RESET doesn't touch it: PLAY
keeps rolling, the head stays where it was. That's also why the transport
keeps winding while the front panel is on STOP: `Machine::tickCassette()`
paces it by CPU cycles while running and by real elapsed time while not,
splicing the two with no jump so 25×/50× (turbo included) still track exactly
right when the CPU *is* running. See `ALTAIR_REVIEW.md` §3.2b/§3.4. Altair
4K/8K BASIC come from `roms/fetch-basic.sh` (pinned + checksummed; CI runs it
before deploying).

## Disks and CP/M

The **MITS 88-DCDD cabinet** below the front panel is two 8-inch floppy drives
on ports `0x08`–`0x0A`. Click a drive to open **Insert Diskette** — pick from
`disks/manifest.json`, choose a local `.dsk`, or insert a blank one. **Blank
(Unformatted)** is a 0xE5-filled raw image, exactly like real 8-inch media out
of the box: boot CP/M elsewhere and run `FORMAT` on it first. **Blank
(Formatted)** carries real MITS sector framing with its directory pre-wiped,
so CP/M can read and write it right away — then press **BOOT** on the
cabinet (or do it from the panel: `EXAMINE` `0xFF00`, `RUN`). `machine.bootDisk()`
drops the 256-byte MITS bootstrap PROM at `0xFF00` and starts there; it reads
track 0 of drive A and hands off to the diskette's cold-start loader.

**LOAD SPEED** in the cabinet's top bar paces rotation: a real 88-DCDD spins
at ~166 ms/revolution over 32 sectors (~193 sectors/sec), which `IN 0x09`
(the sector-position register) now gates on -- **Realistic** is the default,
matching the paper-tape/cassette pattern (5×/25×/50× for the impatient, an
internal **Max** for `?test=1`). A cold CP/M boot at Realistic runs a couple
of seconds longer than at Max; the CPU itself still always runs at real
2 MHz regardless. See `ALTAIR_REVIEW.md` §3.2d.

Standard MITS images are 337,568 bytes (77 × 32 × 137). `*.dsk` is git-ignored;
all five (CP/M, WordStar, Zork, the games disk, Altair DOS) are fetched pinned
and SHA-256 checked by `disks/fetch-disks.sh` — see that script's header and
`disks/README.md` for the redistribution-rights picture, which is genuinely
mixed across the five. CP/M needs the full 64 KB
of RAM, which the machine has (a period build got there with four 88-16MCD 16 KB
dynamic-RAM boards).

A drive's red lamp flickers on head-load, seek, and data transfer. Writes
(`SAVE`, `ED`, `PIP`) go to the in-memory image; a **SAVE** button appears on a
drive with unsaved changes and downloads the modified `.dsk`. **RESET** re-runs
the disk boot with the diskettes still in their drives.

Each loaded drive has a **?** button — a "How to use" dialog with that disk's
programs (how to start and quit them) plus a CP/M command primer. The text
comes from the `help` / `os` fields in `disks/manifest.json`.

## Cassette (88-ACR)

The **88-ACR deck** (ports `0x06`/`0x07`) appears when a preset includes it.
Click it to open the picker: a library tape (`tapes/manifest.json`; `startrek.cas`
ships), a `.cas` file you saved before, or **Insert Blank Tape**. A library
entry's `cload` field is the one-character BASIC file name — the deck's hint
then reads `CLOAD "S"` instead of a placeholder. The deck is styled after the
audio-cassette recorder a hobbyist wired to the 88-ACR (a Radio Shack unit was
the common choice): the cassette turns behind a smoked window, a bar tracks the
head, and a piano-key transport **REW / PLAY / F.FWD / STOP / REC / EJECT**.

The "tape" is one linear byte stream (`CassetteACR`) with a head at `pos()`;
past the recording is blank tape, and `capacity()` is the physical end. It
behaves like a real cassette:

- **PLAY just plays.** The head rolls forward at the selected byte rate whether
  or not BASIC is reading the board — press PLAY with no `CLOAD` pending and the
  counter still climbs, the bytes just go by unheard (the ACR has a one-byte
  buffer). `tick()` credits byte-times to `credit_`; `IN/OUT 0x07` spend it, so
  BASIC's `CLOAD` / `CSAVE` loop can move several bytes per frame and 25×/50×
  really are that fast. Recording needs **PLAY + REC** together, like the real
  interlock.
- **F.FWD / REW** wind the head at `kWindMult`× the byte rate; hitting the start
  (REW) or the physical end (F.FWD / PLAY) **auto-stops the transport and pops
  the key up**.
- **Several programs fit on one tape.** `CSAVE` records *at the head*, not from
  zero — after one save the head sits past it (no auto-rewind), so pressing REC
  again appends the next program (with a short blank gap between). To read one
  back: **REW**, `CLOAD "name"`, **PLAY** — or `CLOAD` the first, which leaves
  you positioned to `CLOAD` the next.
- The **counter** is a 3-digit mechanical unit: `pos / 16`, rolling 999→000.
  **Click it to zero it** at the start of a tape and note where each program
  begins.

The transport moves the head on the CPU cycle clock (`tick()`), so the tape is
paused while the front panel is STOPped — nobody tapes with the CPU halted.
There's no audio model, but the status-bit polarity matches what our 8K BASIC
ROM polls (`cassette.cpp`).

**SAVE** — after `CSAVE`, a **SAVE .CAS** button glows and opens a real *Save As*
dialog (`window.showSaveFilePicker`, Chrome/Edge; a plain download elsewhere); it
writes the whole tape, every program on it.

The **TAPE SPEED** selector paces the whole transport: *Realistic* is the real
300 baud (Star Trek really does take ~10 minutes), then *5× / 25× / 50×* — even
50× is ~12 s for Star Trek, so none of it is instant. Remembered in
`localStorage`. `?test=1` and preset **Auto-load** use an internal unlimited
speed that isn't in the picker. The Cassette Hobbyist preset threads Star Trek
here; once 8K BASIC is up you `CLOAD "S"`, press PLAY, then `RUN`.
