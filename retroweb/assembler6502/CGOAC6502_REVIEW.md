# CG-OAC-6502 — hardware findings and citations

This document records what the emulator's behavior is actually based on:
netlist evidence from `cpu6502/pcb/pcb6502/FullBoard/pcb6502full.net`
(exported from `pcb6502.kicad_sch`), cross-checked against the real firmware
in `cpu6502/rom/`, and the decisions made where the two didn't agree. Same
role as `retroweb/altair8800/ALTAIR_REVIEW.md` §7.1 for that board: a schematic
discrepancy is documented with its evidence trail, not silently "fixed" or
silently reproduced.

## 1. Populated parts

The netlist's `value` field is cosmetic schematic text; the real part comes
from each symbol's `libsource`. All three programmable chips are the WDC
**CMOS** parts, not their NMOS/MOS ancestors:

| Ref | Value text | Actual libsource part |
|---|---|---|
| U1 | `6502 (CPU)` | `PCM_65xx-library:W65C02SxP` |
| U4 | `6522 (VIA)` | `PCM_65xx-library:W65C22NxP` |
| U7 | `6551 (ACIA)` | `PCM_65xx-library:W65C51NxP` |
| U2 | `28C256 (ROM)` | `Memory_EEPROM:28C256`, 32K EEPROM, ZIF socket footprint |
| U3 | `62256 (RAM)` | same 28-pin symbol/footprint family, populated as a 62256 SRAM |
| U9 | `DS1813` | `Power_Protection:DS1813` reset supervisor |

This matters: `rom/bios.s` is `.setcpu "65C02"`, and the CMOS parts fix
several documented NMOS errata (65C02: `JMP ($xxFF)` page-wrap bug,
decimal-mode ADC/SBC flags; W65C51: NMOS 6551's inaccurate baud-rate
generator). The core implements the CMOS behavior throughout — see
`cpu65c02.cpp`'s file header for the full list.

## 2. Address decode (U5, a 74HC00 quad NAND)

Traced net-by-net from `pcb6502full.net`:

- Gate 4 (pins 12,13→11), both inputs tied to `/A15`: `CS_EEP = NAND(A15,A15)
  = /A15`, wired to U2 (ROM) `~CS`. ROM lives at **$8000-$FFFF**. Matches
  `gall_oac.cfg` exactly: `BASROM` at `$8000` size `$7E00`, `WOZMON` at
  `$FE00` size `$1FA`, `RESETVEC` at `$FFFA` size `6` — sums to exactly
  `$8000` bytes.
- Gate 2 (pins 4,5→6): `CS2 = NAND(A14, CS_EEP)`, active when A15=0, A14=1
  (`$4000-$7FFF`). Feeds both U4 (VIA) `~CS2` and U7 (ACIA) `~CS2`.
- VIA `CS1` (active high, pin 24) is wired to `A13`; ACIA `CS1` (pin 2) to
  `A12`. So: **VIA at `$6000-$7FFF`** (A13=1), **ACIA at `$5000-$5FFF`**
  (A12=1) — each an 8K window, heavily mirrored (VIA only decodes 16
  registers off A0-3; ACIA decodes 4 off A0-1). Matches `rom/via.s`'s
  `PORTB = $6000` and `rom/bios.s`'s `ACIA_DATA = $5000` exactly.
- Gate 3 (pins 9,10→8): `CS_RAM = NAND(Phi2, CS_EEP)`, active whenever
  A15=0 during the valid part of the bus cycle — **no A14 term**, so as
  literally wired this selects RAM across the *entire* lower 32K, not just
  a RAM-only sub-range.
- **Gate 1 (pins 1,2→3) is completely unused** — input pin 2 floating,
  output pin 3 floating. This is the strongest evidence for what follows:
  a spare gate that most plausibly should have supplied a missing "exclude
  A14" term and never got wired in.

### 2a. RAM (U3) control-signal erratum — corrected

The netlist wires U3 pin 22 (`~OE`, output enable) to `R/W`, and U3 pin 27
(`~WE`, write enable) to `A14` — not the standard `~WE ← R/W`. Taken
literally, combined with §2's un-gated `CS_RAM`: for any address below
`$4000` (A14=0), `~WE` is permanently asserted whenever the chip is
selected, regardless of whether the CPU intends a read or a write, while
`~OE` is asserted only during what the CPU considers a *write* (R/W=0) —
backwards. Zero-page and stack reads would be corrupted by phantom writes
of whatever's floating on the bus; the machine could not boot as drawn.

**Decision (confirmed with the board's owner):** treat this as a schematic
export error, not a real, working design — emulate the corrected,
functional wiring (`~WE ← R/W`, output enabled whenever selected) **and**
narrow RAM's decoded range to **`$0000-$3FFF`** (excluding the VIA/ACIA
window `$4000-$7FFF` a corrected design would need gate 1 to exclude).
Independent evidence this is the right range: `rom/bios.s`'s own boot
banner (the `ggeretsae` easter egg) advertises **"16K RAM"** verbatim, and a
real boot of the actual firmware through this emulator auto-detects
**15615 BYTES FREE** in BASIC — a believable figure for 16K minus the
zero-page/stack/buffer overhead below it, and nowhere near what a bug in
the corrected range would produce. Implemented in `bus.cpp`.

### 2b. VIA/ACIA overlap at $7000-$7FFF — kept as a documented latent bug

VIA only checks A13; ACIA only checks A12; neither excludes the other's
bit. At `$7000-$7FFF` (A13=1 *and* A12=1), both chips' selects assert
simultaneously — real bus contention on a real board. Firmware never
addresses this range (`rom/via.s` uses `$6000-$600E`, `rom/bios.s` uses
`$5000-$5003` exclusively), so it's never fired in practice.

**Decision (confirmed):** keep this — it isn't excluded, it's modeled.
`bus.cpp` flags `last_access_was_contended` and resolves the read as a
wired-AND of both chips' output (a common, but explicitly *approximate*,
stand-in for two CMOS totem-pole outputs fighting on one line — not a
claim of exact electrical behavior). `bus_test.cpp` pins both that this
never fires in the ranges real firmware touches, and what happens when
something deliberately pokes `$7000-$7FFF`.

## 3. Clocking

X1 is a canned 1 MHz oscillator module (not a bare crystal — it drives
`PHI2` directly into the CPU, VIA, and ACIA, and gates `CS_RAM`). Real
1 MHz, no turbo, per `retro/CLAUDE.md`. Y1 (1.8432 MHz) feeds the W65C51's
internal baud-rate generator directly — `acia65c51.cpp`'s `baud()` decodes
the *live* CTRL register into a real bps figure (WDC datasheet Table 4) so
the terminal can meter output against the ACIA's actual configured rate,
not a fixed per-profile guess.

## 4. Reset (DS1813, U9)

Open-drain `RST`, shared by SW1 (front button — works because it's
open-drain, just shorts the node), J4 (external reset header), and the
CPU/VIA/ACIA reset pins. Dallas/Maxim DS1813 datasheet: ~150-200ms
power-on reset hold. Modeled as a 175ms cycle-count hold in `machine.cpp`
before the CPU's first real vector read — paced across `run_cycles()`
calls (see the "reset hold pacing" fix below), not burned instantly.

## 5. LEDs — corrected during implementation

An earlier pass at this document (before the C++ implementation) concluded
D1/D4/D7 were VIA-driven, based on the netlist's `RNPA0/1/2` net names
looking related to VIA's own `PA0/1/2`. **That was wrong** — direct netlist
inspection shows `/PA0`, `/PA1`, `/PA2` and `/RNPA0`, `/RNPA1`, `/RNPA2` are
two entirely separate, unconnected nets. All three LEDs are simple, always-
passive circuits, no VIA/CPU involvement:

- **D1 "Power"**: `+5V → LED → R8 → GND`. Genuinely just a power indicator.
- **D4 "Rx" / D7 "Tx"**: sit directly across the RS232-*level* RX/TX lines
  (pre-MAX232 for RX off U8 pin 13, post-MAX232 for TX off U8 pin 14)
  through a resistor to GND — real line-activity indicators, independent
  of firmware entirely.

`machine.h`'s `on_power_led`/`on_rx_led`/`on_tx_led` model this: D1 fires
once, permanently, at construction; D4/D7 pulse per received/transmitted
byte (a real diode on the line would flicker per bit transition, not per
byte — a labelled simplification, since modeling literal line voltage
wasn't worth it for a UI indicator).

## 6. Jumper headers

- **J7 "Interrupts"**: breaks IRQ/NMI out from VIAIRQ/ACIAIRQ separately;
  R1/R2 (3.3kΩ) pull the CPU's IRQ/NMI pins high, so *no* jumper means
  neither chip's interrupt reaches the CPU at all. Confirmed from
  `rom/bios.s` itself (not inferred): `IRQ_HANDLER` reads the VIA's
  `T1CL`; `NMI_HANDLER` reads `ACIA_STATUS`/`ACIA_DATA`. Default:
  **VIA→IRQ, ACIA→NMI** — matches the firmware exactly.
- **J5 "Boot"**: PA1-4, wired up but not read anywhere in the shipped ROM
  (`BOOT`'s loop reads the serial port, not PORTA). Modeled, inert.
- **J8 "RS232 CTS"**: optional jumper routing the ACIA's own RTS output
  through the MAX232's spare channel to the connector. The ACIA's hardware
  `~CTS` input is hard-tied to GND — always clear-to-send, no incoming
  hardware flow control by default. Default: open.

## 7. J3 LCD accessory — default changed after running the real ROM

`rom/via.s` contains a complete HD44780 driver (E/RW/RS on VIA PA7/6/5,
data on PB0-7) — written for the sibling `pcb6502-LCD` board, but J3
exposes the identical VIA pins on FullBoard, so a real LCD plugged into J3
works identically. Running the actual, unmodified `gall_oac.bin` through
this emulator surfaced something the schematic alone didn't: `rom/bios.s`'s
`RESET` unconditionally calls `reset_via_irq`, which busy-polls the LCD's
status bit on PB7 *before doing anything else* — with nothing driving that
pin, a genuinely bare FullBoard would hang forever at boot, on real
hardware, not just in emulation.

**Decision (confirmed):** the accessory (`hd44780.h`/`.cpp`) is **attached
by default** so the stock ROM actually boots, with `Machine::
set_lcd_attached(false)` reproducing the real bare-board hang on demand.

## 8. `rom/make.sh` — fixed, not replaced

Initial investigation concluded there was no script linking `bios.s` +
`wozmon.s` + `msbasic.s` into one image. That was also wrong: `msbasic.s`
already pulls in `bios.s` transitively (`msbasic_galloac/extra.s` does
`.include "../bios.s"` under `.ifdef GALL_OAC`, which is set only when
`ca65` is invoked with `-D gall_oac`), which pulls in `via.s`/`commands.s`/
`format.s`/`vterm.s`/`load.s`/`wozmon.s` — genuinely one linked image
already. `rom/make.sh` just had a two-line shell typo (`$i = gall_oac`
instead of `i=gall_oac`, plus a stray `done`) that made the script itself
fail to run. Fixed; `make -C retroweb/cg-oac-6502 rom` now produces a
clean 32768-byte image with zero assembler errors, and it boots correctly
through the emulator end-to-end (see `tests/machine_test.cpp`'s
`BootsTheRealFirmwareToTheAdvertisedBootMenu`).

## 9. CPU core validation

`cpu65c02.cpp` passes both of Klaus Dormann's independent 6502/65C02
functional test suites in full (`make dormann` — ~30M and ~22M instructions
respectively), including the 65C02-extended-opcode suite's internal
decimal-mode ADC/SBC self-check. This is the same validation role
`TST8080`/`CPUTEST`/`8080EXM` play for the Altair's i8080 core.

## 10. A real bug this test suite caught before it shipped

`Machine::step_one()` originally called `cpu.reset()` to end the DS1813
hold, and `cpu65c02::Cpu::reset()` (correctly, matching `i8080::Cpu::
reset()`'s precedent) zeroes its own `cycles` counter — a real 65C02
doesn't remember T-states across a reset either. But `Machine::run_cycles()`
was using `cpu.cycles` as its own wall-clock-pacing budget target, so a
reset landing mid-call made that target retroactively unreachable-then-
overshot: `machine_test.cpp`'s `ResetHoldsPcAtZeroUntilTheDs1813DelayElapses`
caught this immediately (pc came back wrong after the hold). Fixed by
giving `Machine` its own monotonic `total_cycles_`, independent of
`cpu.cycles`, for pacing purposes.

## 11. Open items (not blocking, flagged)

- Exact AT28C256 page size / write-cycle timing in `eeprom28c256.h` should
  be spot-checked against the specific chip's datasheet if a ROM-burning
  UI is ever reintroduced (see §12 below — the browser ROM-programmer panel
  itself has since been removed; `m.burnRom()` still seats the compiled
  image once at boot).
- LED-driving PA0-3 usage (if any exists elsewhere beyond what's been read
  so far) hasn't had a final confirming pass — §5 above is based on the
  netlist wiring, which is conclusive on its own regardless.

## 12. The Wozmon + resident-assembler pivot

The ROM's software identity changed completely: the earlier MSBASIC port
(`msbasic_galloac/`, cloned from `mist64/msbasic`) and its numbered boot
menu, clock utility, and BASIC-token custom commands (`commands.s`,
`format.s`) are gone from the build entirely — dropped, not merely
unreferenced, so the license-provenance open item this section used to
carry about `msbasic_galloac` no longer applies. In their place: boot goes
straight to a fixed Wozmon, plus a new resident line editor and two-pass
assembler (`editor.s`) that run on the emulated 65C02 itself. The real
hardware model (`cpu65c02`, `via65c22`, `acia65c51`, `eeprom28c256`,
`hd44780`, `bus`) is untouched by this — only ROM software changed.

**The `START_WOZ` entry-point bug.** The ROM this project inherited aliased
`START_WOZ` directly onto `NOTCR` (`wozmon.s`) — Wozmon's *mid-loop* "was
that a CR" branch, which does an unconditional `INY` on whatever Y already
holds on entry. That only ever worked because the old boot menu's
`boot_wozmon:` path happened to reach it right after a `JSR CHRIN`, where Y
was still valid input-buffer state. Once `RESET` jumps straight to Wozmon
with no menu in between, entering at `NOTCR` is genuinely unsafe: Y is
whatever `RESET`'s own init left in the register, unrelated to any text
buffer. Fixed by moving `START_WOZ` down to `ESCAPE`, Wozmon's real cold/
re-sync entry point — the classic Apple-1 Wozmon source's actual "start
over" label, which prints the authentic `\` banner and explicitly sets
`LDY #$01` before its read loop, so it self-initializes regardless of
incoming A/Y. See the comment at `wozmon.s`'s `START_WOZ` label for the
in-source version of this note.

**VIA init.** `reset_via_irq`'s Timer-1 free-run setup (`ACR`/`T1CL`/`T1CH`,
arming a periodic IRQ) existed only to drive the old jiffy-clock IRQ
counter, which left with the clock utility. `via.s`'s init (renamed
`reset_via`) now only sets up GPIO direction and the LCD, and explicitly
disables every VIA interrupt source (`IER = $7F`) rather than arming one by
default — a user's own assembled program can still arm CA1/Timer1/etc
itself via `IER` if it wants VIA interrupts, routed through J7 exactly as
before (see `IRQ_HANDLER` in `bios.s`).

**Memory map for the editor/assembler**, chosen to respect the only two
existing claimants on RAM (`bus.cpp`'s `if (addr < 0x4000) return
ram[addr]` — RAM is $0000-$3FFF, full stop) — Wozmon's own `WOZMON_BUFFER`
at $0300-$03FF and the ACIA RX ring at $0200-$02FF:

| Region | Range | Size |
|---|---|---|
| Zero page | $0000-$00FF | 256 B |
| Stack | $0100-$01FF | 256 B |
| ACIA RX ring | $0200-$02FF | 256 B |
| Wozmon buffer | $0300-$03FF | 256 B (128 used) |
| Object code | $0400-$2BFF | ~10.25 KB |
| Symbol table | $2C00-$2FFF | 1 KB |
| Source text | $3000-$3FFF | 4 KB |

With BASIC gone, zero page is otherwise wide open — the editor's own ZP
vars only need to avoid Wozmon's ($24-$2B) and `load.s`'s ($4B, $4D-$4F).

**Serial LOAD/SAVE** (`load.s`, rewritten — see the file's own header
comment): LOAD frames itself with an idle timeout (real hardware has no
"end of transfer" marker on the receive side, since a human or an
arbitrary text file is doing the sending) — once ~0.33 s passes with no new
byte after the first one arrives, the buffer is considered complete and
NUL-terminated. SAVE is the browser-initiated direction, so it can use
exact framing instead: ASCII STX ($02) before, ETX ($03) after — genuine
"Start/End of Text" control-code framing, not a project invention. Both
are reached from Wozmon exactly like any other resident routine, via
`<entry-addr>R` — LOAD and SAVE are not new syntax, just new addresses.

**No return path from `RUN`.** Wozmon's `R` command is `JMP (XAML)` and
never returns, so a stray `BRK` in an assembled test program needs
somewhere sane to land. Documented convention (also shown on the browser's
Help panel, generated from the real build's addresses so it can't go
stale — see below): end a test program with an explicit `JMP` back to
`START_WOZ`, the same re-sync entry point RESET itself now uses.

**Browser-side address staleness.** Every ROM include-order change in this
project shifted every entry point's real address (`NEW_ENTRY`,
`ENTER_ENTRY`, `LIST_ENTRY`, `ASM_ENTRY`, `LOAD_ENTRY`, `SAVE_ENTRY`,
`START_WOZ`), and each shift required hand-updating hardcoded hex literals
across the GoogleTest suite, the Playwright suite, and (new in this phase)
the browser's own Help panel and Save/Load flow. Rather than hand-maintain
a fourth copy of these addresses in `app.js`, `web/gen_entrypoints.py`
parses the real linker-generated `firmware.lbl` at build time and emits
`web/roms/entrypoints.js` (gitignored, like the other build artifacts) —
the Help panel and test suite both read `window.CGOAC_ENTRYPOINTS` rather
than a literal. `SRC_START`/`SYM_START`/`OBJ_START` aren't in that file:
they're plain `=` constants, not linker labels, so they don't appear in
`.lbl` output at all.

**Input pacing, browser side.** Until this phase, `term.onData` fed every
typed/pasted byte to `m.typeChar()` in a tight loop with no pacing at all
— harmless for a human typing at keyboard speed, but a real risk for
LOAD/SAVE and for paste, both of which can hand the terminal many bytes in
one JS turn. The ACIA models a real single-byte RX register: a second byte
arriving before the NMI handler drains the first is a genuine overrun, not
a simulation nicety — the same failure mode this project's own C++ test
harness and Playwright suite have each hit independently before. The fix
(`app.js`'s `driveFrame`) paces input to the ACIA's live configured baud by
default, consistent with this repo's realism rules (`CLAUDE.md`: never
speed up CPU/bus timing, even as an opt-in override) — it does not inject
any cycles beyond the current animation frame's own real, wall-clock-paced
budget; it only slices that existing budget between characters instead of
running it all before or after typing them. The one labelled, opt-in
override is the Save/Load panel's "instant transfer" checkbox (mirroring
the Altair's LOAD SPEED convention cited in `CLAUDE.md`), plus the
existing `?test=1` carve-out that automated tests use so a suite run isn't
paced out at 19200-baud realism on every save/load.

## 13. The command-shell / line-numbered-program follow-on

A second pivot, on top of §12's: the flat "each function is its own
`<addr>R`" surface (`NEW_ENTRY`/`ENTER_ENTRY`/`LIST_ENTRY`/`ASM_ENTRY`/
`LOAD_ENTRY`/`SAVE_ENTRY`) is gone, replaced by a single new address
(`SHELL_ENTRY`) that opens a persistent, BASIC-like command shell with its
own `*` prompt — real line-numbered program storage (insert/replace/
delete by number, no renumbering needed to insert between two existing
lines) plus `NEW`/`LIST`/`LIST n`/`EDIT n`/`ASM`/`RUN`/`LOAD`/`SAVE`/`QUIT`
as typed command words. This is original engineering for this board (not
a recreation of a specific historical product), but its shape mirrors two
well-documented real precedents cited below rather than being invented
from nothing.

**Storage format.** Each program line is `[num_lo][num_hi][ascii
text...][CR]` in the same 4K source buffer (`SRC_START`-`SRC_END`) as
before — a 2-byte *binary* line-number prefix, not ASCII digits in the
buffer. This is a deliberate choice, not the only option: storing the
number as decimal ASCII (matching what's displayed) would avoid a
conversion step on display, but `STORE_LINE`'s sorted-insert scan needs to
compare numbers on every line it walks past, and a 16-bit binary compare
is a couple of instructions where re-parsing ASCII digits every scan would
be many more, repeated on every edit. Decimal is purely a *display*
format — produced on the way out by a new `PRDEC` routine (repeated
subtraction against a `10000/1000/100/10/1` power-of-ten table, the
standard 6502-textbook binary-to-decimal shape, e.g. Leventhal's *6502
Assembly Language Programming* — the direct decimal analogue of this ROM's
own `PRBYTE`/`PRHEX` hex routines, wozmon.s) and parsed once on the way in
by `DEC_PARSE`. End-of-buffer sentinel: a stored `num_lo=0, num_hi=0` —
line number 0 is reserved/disallowed as a real line number for exactly
this reason (a real line whose number happens to be, say, 256 has
`num_lo=0` and must not be mistaken for the sentinel; checking both bytes,
not just one, is what makes that safe).

**`STORE_LINE`, the single mutation primitive.** Every write path
(explicit numbered entry, `EDIT`'s retype, and `LOAD`'s per-line
ingestion, below) funnels through one routine: find any existing line
with this number and delete it (a real byte-shift `MEMMOVE_DOWN`, closing
the gap), then, only if new text was given, insert it at the correct
sorted position (`MEMMOVE_UP`, opening a gap). Doing delete-then-insert as
two separate, simpler shifts — rather than one "resize in place" op —
avoids needing separate net-growth-vs-shrink math; each step's own bounds
check is correct on its own. This shift-the-buffer-to-open-or-close-a-gap
approach isn't a project invention either: it's the same algorithm classic
Microsoft BASIC derivatives (Applesoft, Commodore BASIC, and this repo's
own now-retired `msbasic_galloac`) use to insert or delete a program line
in their own tokenized-line buffers — a real, well-documented precedent
for exactly this data structure on exactly this class of machine.

**A real bug this design caught the hard way: `MVDST` is a working
pointer, not a return value.** The first version of `STORE_LINE` tried to
reuse `MVDST` (the destination pointer `MEMMOVE_UP` had just been given)
as "the new end of the buffer" after the insert, to place the fresh 0,0
sentinel. That's only correct in the degenerate append case — where
`MVLEN` is 0 and `MEMMOVE_UP` returns immediately without touching
anything. `MEMMOVE_UP` actually *walks `MVDST` backward to zero* as its
own loop counter once there's real data to shift (inserting before or
between existing lines, not just appending), so reusing it afterward reads
whatever the copy loop left it at, not the address it started at. Every
early test happened to only ever append (strictly increasing line
numbers), so this shipped looking correct before a single test inserting
a line *before* existing ones (`InsertingBetweenExistingLinesNeedsNoRenumbering`,
`tests/editor_test.cpp`) caught it: the newly-inserted line's own
terminating CR and the following line's number bytes were silently
zeroed. Fixed by using the new end position already computed into
`TEMP16` during the bounds check (`ENDPTR_before_insert + NEWTOTLEN`),
which `MEMMOVE_UP` never touches — see the comment at `STORE_LINE`'s
`sl_donetxt` label (`editor.s`).

**`LOAD`/`SAVE` now speak real decimal text, not the internal binary
format.** A saved program has to be a genuinely readable text file (open
it, edit it, re-import it), so `DO_SAVE` converts each line via `PRDEC` on
the way out (`"<num> <text>\r"`, still framed in the same real STX/ETX
control bytes as before), and `DO_LOAD` splits the incoming stream on CR
and feeds each completed line through `PROCESS_LINE` — the *exact same*
classify-and-store path the interactive shell prompt itself uses. This one
reuse is what makes `LOAD` accept both a real numbered save file and a
plain unnumbered text file a person hand-typed and imported: an unnumbered
line just auto-numbers, exactly as it would if typed directly at the `*`
prompt (`AUTO_NUMBER`: highest existing number + 10, the same step
classic BASIC's own `AUTO` command used).

**`LOADMODE` — a real hijack risk this design has to guard against.**
Because `LOAD`'s per-line ingestion reuses the shell's own command
dispatcher, a plain-text import whose content happens to read `RUN` or
`QUIT` on its own line would, without a guard, be misread as *that
command* mid-transfer — jumping into the object-code region or back to
Wozmon partway through a load, silently truncating it. `PROCESS_LINE`
takes a `LOADMODE` flag (set only while `DO_LOAD` is actively receiving)
that skips command-word matching entirely and always auto-numbers instead
while it's set — verified by
`Load.LoadDoesNotLetAnUnnumberedLineThatReadsLikeACommandHijackTheTransfer`
(`tests/load_test.cpp`), which imports a file containing a bare `RUN` line
and confirms all three lines land in the program, in order, rather than
the transfer being hijacked partway through.

**`ASM`'s error report now names the real program line, not a physical
count.** The pre-shell `ASM_ENTRY` reported a 1-based sequential line
count (`ERR LINE $0002`) because that was the only numbering that existed.
Now that every line has its own real, human-chosen number, `ASM_READLINE`
captures it into `CURNUM` as a side effect of reading the line, and
`DO_ASM`'s error path prints that (via `PRDEC`) instead — `ERR LINE 30`
means line 30, not "the second line in the file".

**Browser-side state tracking.** `app.js` types every synthetic keystroke
itself (the human's own typing is unaffected), so it's the only thing that
can know whether the terminal is currently sitting at Wozmon's `\` prompt
or already inside the shell — and it has to know, because blindly sending
`"<hex>R"` (Wozmon's run syntax) into an *already-open* shell prompt would
misparse as a large decimal line number followed by a bad trailing letter,
never reaching Wozmon's dispatcher at all (Wozmon isn't listening once the
shell has taken over `READCHAR`). A simple `inShell` boolean, set on
entering the shell and cleared on `QUIT` or a hardware reset (SW1), gates
a new `ensureShell()` helper that `runSave`/`runLoad` call before typing
`SAVE`/`LOAD` — covered by
`saveload.spec.ts`'s "Save/Load work correctly even when the terminal is
already inside the shell" case.

**Ctrl-C breaks a hung program — a real serial-break convention, not a
cooperative check.** `RUN` transfers control to the user's own assembled
object code with no supervision; a program with a genuine infinite loop
(or one missing its documented resume `JMP`) had no way back to the shell
short of a hardware reset (which also wipes the program). The fix lives
entirely in `bios.s`'s `NMI_HANDLER`, the ACIA's receive-interrupt
handler: it now recognizes byte `$03` (ASCII ETX, the real Ctrl-C wire
value) as it comes off the wire and, instead of buffering it into
`SERIAL_BUFFER` and `RTI`-ing back to whatever was interrupted, resets the
stack pointer and jumps straight to `SHELL_PROMPT`. This is the standard
6502 SBC-monitor break pattern — the receive-ISR itself recognizes the
break character and redirects, rather than relying on the interrupted
code to notice anything. It's why NMI (not a polled check) is the only
mechanism that actually works here: NMI fires the instant the byte
arrives regardless of what the CPU is doing, including a tight `JMP $`-
to-itself loop with no `READCHAR` poll of its own — unlike, say, classic
Microsoft BASIC's STOP key, which only works because the interpreter
polls the keyboard between statements. A resident assembler's own
compiled programs have no such interpreter loop to lean on, so a genuine
interrupt is the only option, matching real serial-break convention on
6502 monitors of the era.

Two deliberate details: the handler never `CHROUT`s anything before
jumping — the interrupted code could itself be mid-`CHROUT` (inside
`CHLL`'s real per-character ACIA delay), and writing a fresh byte to
`ACIA_DATA` before that finishes would corrupt whatever transmission was
already in flight, so the shell's own `*` reprompt is the only feedback
Ctrl-C gives, and it's sufficient. And there's no attempt to preserve or
resume the interrupted context — Ctrl-C is a one-way break, not a
suspend/resume; the stack is reset to a known-good empty state (`LDX
#$FF; TXS`) rather than trying to unwind whatever depth of subroutine
calls the running program had reached, since there's nothing sane to
return to once the user has asked to abandon it. This only works while
the ACIA is jumpered to NMI (J7's default, matching the shipped ROM) —
routing it to IRQ instead already disables all serial reception today
(`IRQ_HANDLER` is a bare stub), a pre-existing limitation, not new here.

Covered by `Editor.CtrlCBreaksAGenuinelyInfiniteLoopBackToTheShell`
(`tests/editor_test.cpp`) — types a real `JMP START`-to-itself program,
confirms via `cpu.pc` that it's genuinely spinning at the loop after a
large cycle budget, then confirms Ctrl-C returns to a working shell
prompt (a follow-up `LIST` still working, proving the stack/state wasn't
left corrupted) — and by
`Editor.CtrlCAtTheShellPromptDiscardsAnyPartialLineAndReprompts`, which
sends Ctrl-C mid-typing (no program running at all, just idle in
`READLINE_ECHO`'s own poll loop) and confirms the abandoned partial line
never gets stored. `editor.spec.ts`'s "Ctrl-C breaks a genuinely hung
(infinite-loop) program back to the shell" drives the same scenario
through the real browser terminal — no `app.js` change was needed for
delivery, since xterm.js's default keybinding already sends the raw `$03`
byte via `term.onData` on Ctrl+C with nothing selected, flowing through
the existing input-pacing pipeline unmodified.

**`SHELL_ENTRY` pinned to a fixed, memorable `$8000`, not wherever the
build happens to land it.** Every other address in this ROM is looked up
dynamically at build time (`gen_entrypoints.py` reads the real value out of
`tmp/firmware.lbl`, and the browser/tests read it back from that generated
file) precisely so a source edit anywhere in `editor.s` can freely shift
every label after it without anything going stale. `SHELL_ENTRY` is the
one address a human is expected to type from memory, over and over, at
Wozmon's raw `\` prompt — every other address's saving grace (never
hand-typed) doesn't apply to it. `gall_oac.cfg`'s `BASROM` region starts
at `$8000`, and the old BASIC-era segments (`HEADER`, `VECTORS`,
`KEYWORDS`, …) that used to share that region are gone (dropped in §12's
pivot, still declared in the linker config but empty — nothing left in
the build populates them), so the `BIOS` segment is now the first thing
placed in ROM. That made a plain, no-cost fix possible: a one-instruction
`jmp SHELL_START` trampoline, literally the first bytes of `bios.s`'s own
`.segment "BIOS"`, pinned at `$8000` by construction; the real shell
implementation (renamed `SHELL_START`, since the label `SHELL_ENTRY`
itself now names the trampoline) keeps moving freely as `editor.s`
changes, exactly as before. Same shape as a fixed low-memory cold-start
vector on period hardware — e.g. the Commodore 64's BASIC cold-start entry
sitting at a fixed `$A000` despite KERNAL/BASIC ROM internals shifting
between revisions. `gen_entrypoints.py`'s `WANTED` list still just says
`"SHELL_ENTRY"` — it resolves to whatever address that name has in the
freshly-built `.lbl` file, so this required no change there, in `app.js`,
or in the Playwright specs (all read `E.SHELL_ENTRY` live); only the three
GoogleTest files' own hardcoded `kShellEntry` constant (documented in each
file as "verified against `tmp/firmware.lbl` after each build," the
established convention for this one intentionally hand-typed address)
needed updating to `0x8000`.

**A fixed OS-call jump table, same trick, for terminal/LCD I/O from a user
program.** A real program has legitimate reason to print something — it's
the only way to *prove* it ran, short of dropping into Wozmon and
examining memory by hand — but every candidate routine (`CHROUT`,
`STROUT`, `print_char_lcd`, `clear_lcd`, `cursorLine1_lcd`/
`cursorLine2_lcd`) already lived at a real but unpinned address, meaning a
`JSR` to one in a saved program could silently break on the next ROM
rebuild. `bios.s`'s trampoline grew from the single `SHELL_ENTRY` jump
into a small table of them — `PRINT_CHAR`/`PRINT_STR` (terminal),
`LCD_PUTC`/`LCD_PUTS`/`LCD_CLEAR`/`LCD_LINE1`/`LCD_LINE2` — each a
3-byte `jmp` to the real (still free-floating) implementation, so
`$8000`-`$8017` is now a small, permanent public API surface. `LCD_PUTS`
required a genuinely new routine (`PRINT_STR_LCD`, `via.s`) — the LCD only
ever had a per-character primitive; the string version is just a loop
over it, same shape as `STROUT`'s own loop, sharing `STROUT`'s `ADDR_PTR`
zero-page pointer (safe: the two never run concurrently). Because this
table now occupies the first bytes of ROM, everything that used to sit at
`$8000` (the old boot code, and `SHELL_ENTRY`'s own trampoline) shifted
down — `SHELL_PROMPT`, the resume-`JMP` target documented on the Help
panel, moved from `$80C5` to `$80EE` in the same rebuild. This is exactly
the kind of drift the whole pinning exercise exists to make harmless:
`gen_entrypoints.py`'s `WANTED` list picked up the seven new names by
name, not address, so the browser and Playwright specs needed no changes;
only tests holding a raw address literal for something *not* on the
pinned list — `RunResumeLandsBackInTheShellNotRawWozmon`'s `JMP
$SHELL_PROMPT` construction, and `board.spec.ts`'s PC-range assertion
against `via.s`'s `lcdbusy` loop (used to prove the bare-board LCD hang
genuinely spins forever, not crashes) — needed updating. The former now
reads the real address at test time straight out of `tmp/firmware.lbl`
(a small `labelAddr()` helper, mirroring what `gen_entrypoints.py` already
does for the browser) instead of hardcoding it a second time, so it can't
go stale on a future rebuild the way it just did on this one.

Covered by `Assembler.JsrPrintCharReachesTheTerminal`,
`Assembler.JsrPrintStrReachesTheTerminal` (pokes a string into RAM byte by
byte first, the way a real program has to — see below), and
`Assembler.JsrLcdPutcAndPutsReachTheLcd` (`tests/assembler_test.cpp`) —
each types a real program that `JSR`s the fixed address, runs it, and
checks the real side effect (`on_serial_out` capture, or `m.lcd.text[][]`
directly). `saveload.spec.ts`'s "shows this build's real OS-call
addresses" mirrors the existing `SHELL_ENTRY`/`SHELL_PROMPT` Help-panel
test for the seven new IDs.

**No way to embed a string literal — v1 scope, not an oversight.** This
assembler has no `.BYTE`/data directive (documented in "v1 assembler
scope" from the start); a program that wants to call `PRINT_STR`/
`LCD_PUTS` has to build its string byte-by-byte with its own `LDA #$xx` /
`STA` sequence into scratch RAM first, then point A/Y at it — genuinely
more typing than a real assembler would ask for, but consistent with
every other v1 cut, and documented as such on the Help panel rather than
left for a user to discover by a confusing `ERR` from a syntax this
assembler was never going to accept.

**`PRINT_ENTRY` reformats stored source into aligned columns for
display — a display-only pass, not a stricter grammar.** The user asked
for classic assembly-listing formatting (label / mnemonic / operand /
comment, each in its own column) without forcing column-sensitive typing
— real teletype-era assemblers split on whitespace, not fixed columns
(this ROM's own toolchain, ca65, works the same way), and re-imposing
strict columns on entry would fight the shell's existing free-form
`STORE_LINE`/`AUTO_NUMBER` design for no real benefit. So the *stored*
text stays exactly what was typed (however many spaces, wherever); only
`PRINT_ENTRY` — shared by `LIST` and `EDIT`'s "show the line" step —
re-derives field boundaries for display, using the same label-scan shape
`PARSE_LINE` already uses (scan for a leading identifier terminated by
`:`) copied onto a display-only buffer copy so it can't disturb `ASM`'s
own use of `LINE_BUF`/`CURSOR`/etc. It deliberately skips all of
`PARSE_LINE`'s real validation (`MNEM_LOOKUP`, addressing-mode
resolution) — an unassembled, even syntactically broken, line still lists
sensibly, matching the old raw-dump behavior's tolerance for garbage.
`LABEL_FIELD`/`OPERAND_FIELD` (8/10 columns) are the only two knobs;
a label or operand longer than its field just gets one guaranteed
separator space instead of breaking alignment further right, rather than
being truncated.

Comments themselves were **already-implemented, untested code** —
`ASM_READLINE`'s "in a comment" flag (`TEMP16`, reused) already stripped
everything from a `;` to end-of-line before `PARSE_LINE` ever saw it, and
the file's own header had documented `LABEL: MNEMONIC OPERAND ; comment`
as the real grammar since this shell's original design — but nothing
exercised it until this pass. `PRINT_ENTRY` is the only place that
*keeps* a comment (this is a display pass, not the real parse) — pads the
operand field out to `OPERAND_FIELD`, then prints `"; "` plus the
comment text. A `;` with nothing real before it (no mnemonic at all, just
`"; comment"` after a possible number) prints as blank mnemonic/operand
placeholder fields followed by the aligned comment — the same "a
comment-only line assembles as a no-op" behavior `ASM_READLINE`'s
stripping already gave for free, just also displaying sensibly instead of
looking like an empty line.

Covered by `Assembler.InlineCommentsAreIgnoredByTheAssembler` and
`Assembler.AFullLineCommentAssemblesAsANoOp` (proving the pre-existing
strip logic, checking real emitted object bytes) and
`Editor.ListFormatsLabelMnemonicOperandAndCommentIntoAlignedColumns`
(proving the new display formatting, including the comment-only-line
case) in the respective GoogleTest files. This reformatting also changed
every existing `LIST`/`EDIT` output string in both the GoogleTest and
Playwright suites — none of the *typed input* in any test changed (still
whatever a human would naturally type), only the assertions checking
what came back, now with the real column padding baked in.

**A real bug: `LOAD` overwrote each incoming line in place instead of
listing them.** `READCHAR` (`bios.s`) echoes every raw byte it reads,
unconditionally, for every caller — including `DO_LOAD`'s own ingestion
loop, which reads the wire format's bare-CR line separators (`load.s`'s
own header explains why the wire format is bare-CR, not CRLF: it matches
what `SAVE` emits and what `PROCESS_LINE` expects). A lone CR on a real
terminal returns the cursor to column 0 *without* advancing to the next
row — so each newly-echoed line landed on top of the previous one instead
of below it, exactly the garbled, overlapping text reported live (a `SAVE`
followed immediately by re-`LOAD`ing the same two lines showed only a
single corrupted row where two were sent). Fixed by having `dload_gotcr`
emit an explicit `LF` right after the CR `READCHAR` already echoed —
mirroring the fix `READLINE_ECHO` already applies after its own CR, for
the identical reason (the interactive prompt's line-input echo has always
had this right; `DO_LOAD`'s echo, added later, hadn't).

Covered by `Load.LoadEchoesEachIncomingLineOnItsOwnTerminalRowNotOverwritingThePrevious`
(`tests/load_test.cpp`) — asserts every CR in a real `LOAD` transfer's
echoed output is immediately followed by an LF, checked against the raw
`on_serial_out` byte capture, not rendered text. Verified as a real
regression test (not just a passing assertion) by deliberately reverting
the `LF` fix and confirming the test fails with the exact garbled output
originally reported (`"10 LDA #$2A20 STA $50*"` — the second line's `20`
literally overwriting the first line's trailing digits), then restoring
the fix. `saveload.spec.ts`'s "Load's own echo lands each incoming line on
its own row, not overlapping the previous one" drives the same scenario
through the real browser terminal — see the note on `innerText()`
reliability below for why it checks a raw output spy rather than rendered
DOM text.

**A real bug: `app.js`'s own `inShell` tracking didn't know about the
*human* entering or leaving the shell.** `ensureShell()` (used by
`runSave`/`runLoad`, per the design note above) only ever got `inShell`
set `true` by its *own* synthetic `"<addr>R\r"` send — never by a human
manually typing that same sequence at Wozmon's prompt, which is exactly
what the Help panel instructs as the normal way in, nor by a human typing
`QUIT` by hand to leave. So a session where the human entered the shell
manually, then clicked Save, sent a *second*, redundant `"8000R\r"` into
an already-open shell prompt — which `PROCESS_LINE`'s own tolerant grammar
(no space required between a line number and following text, so `PARSE_LINE`
falls through to `pl_notext` rather than erroring — see `PARSE_LINE`'s
header) silently accepted as line number 8000, text `"R"`, quietly
inserting a bogus extra line into the program right before Save captured
the buffer. This is exactly what the user's own report ("LOAD is doing
something weird, I'm clicking the Saved program button") turned out to
be — confirmed live by spying on `m.typeChar` and seeing the literal
redundant `"...STA $50\r8000R\rSAVE\r"` sequence app.js actually sent.

Fixed by inferring `inShell` from real signal instead of only from
app.js's own sends: entering is detected from the shell's own banner text
("ASSEMBLY CODER") appearing in the *real ROM output* stream (a small
rolling window, `bannerTail`, checked in `pullSerial()` — the same place
that already drains `m.readOutput()` every frame), and leaving is detected
from the human's own typed line reading `QUIT` (tracked via a `typedLine`
buffer built up in `term.onData`, cleared on Enter/backspace). Both are
real signals: the banner is a byte sequence only the actual shell prints
on real entry (whether reached by a synthetic or a hand-typed `<addr>R`),
and `QUIT` is the literal command that returns to Wozmon regardless of who
typed it. `SW1` (reset) unconditionally clears both trackers, matching the
hardware fact that a reset always drops back to raw Wozmon.

Covered by `saveload.spec.ts`'s "manually entering the shell (as the Help
panel instructs), then Save, doesn't pollute the program with a bogus
re-entry line" — enters the shell exactly the way the Help panel
documents (not via any app.js helper), Saves, then `LIST`s and confirms
exactly two lines exist, no bogus third one.

**A Playwright reliability lesson: `innerText()` can miss content a
screenshot correctly shows.** While chasing the two bugs above,
`page.locator("#screen").innerText()` was observed to return stale
content immediately after an action that a `page.locator("#screen")
.screenshot()` taken at the same moment showed had already rendered
correctly — a distinct unreliability from the pre-existing, already-
documented "WWWWWWW..." glyph-measurement flicker (`editor.spec.ts`'s
existing notes), though the two can compound in the same test run. Since
this project's own established, reliable pattern for exactly this problem
already exists (`terminal.spec.ts`'s CAPS LOCK tests spy on `m.typeChar`
rather than trusting rendered text), the two `LOAD`/`SAVE` tests above
spy on `m.readOutput` the same way (`installOutputSpy`/`getRawOut`,
`saveload.spec.ts`) — both the pass/fail assertion *and* the
synchronization wait (`expect.poll` against the raw spy, not
`toContainText` against the DOM) — so neither test depends on xterm.js
having actually painted anything by the time Playwright looks.

**Prompt/UX polish: `>` instead of `*`, a real newline on every Enter, a
clean reprompt after a program runs, and `Ok` instead of `NEW`/`OK`.**
Four small, user-requested changes, all in the shell's own I/O path
(`editor.s`) rather than any command's logic:

- The prompt character itself changes from `*` to `>` (`shell_loop`'s
  `lda #'>'`) — purely cosmetic, but it's a single literal, so every place
  a test looks for "did the shell reprompt" changes with it (`out.find(">")`,
  GoogleTest; the `/>/g` prompt-count regex, `editor.spec.ts`'s Ctrl-C
  test).
- **A real bug in `READLINE_ECHO`'s blank-Enter case.** The CR/LF-after-CR
  fix already used elsewhere in this ROM (`load.s`'s `dload_gotcr`, and
  `READLINE_ECHO`'s own non-blank path) had an exception carved out for a
  *blank* line: `rl_gotcr` skipped the LF specifically when `Y=0` (nothing
  typed before Enter), on the reasoning that an empty line has nothing to
  echo. But the bare CR itself is still echoed unconditionally by
  `READCHAR` regardless of line length, and a lone CR returns the cursor
  to column 0 without advancing a row — so pressing Enter on its own just
  silently redrew over the current line instead of opening a fresh one,
  unlike every command-line convention where a bare Enter still advances.
  Fixed by dropping the `Y=0` special case entirely — `rl_gotcr` now
  always emits the LF, matching `dload_gotcr`'s unconditional version.
- **`SHELL_PROMPT` now prints its own CR/LF before reprompting; the
  ordinary per-command loop doesn't.** Previously `SHELL_PROMPT` — the one
  address both the interactive loop's own continuation *and* a resumed/
  interrupted program's `JMP` land on (see the fixed-`$8000`-and-jump-
  table note above) — went straight to printing the prompt character with
  no separator, which was fine for the loop case (the prior command's own
  output already ends in its own CR/LF) but wrong for the resume/Ctrl-C
  case: a test program's `PRINT_CHAR`'d output has no reason to end in a
  newline of its own, so the reprompt landed glued onto it (`X*`,
  literally the reported symptom). Splitting the single label into two —
  `SHELL_PROMPT` (prints CR/LF, then falls into `shell_loop`) and
  `shell_loop` (the bare reprompt, used by the loop's own internal
  continuation) — fixes the resume/Ctrl-C case without adding a spurious
  blank line to the ordinary case, since only `SHELL_START`'s startup
  fallthrough and the loop's own `jmp` target `shell_loop` directly,
  while `bios.s`'s `NMI_HANDLER` (Ctrl-C) and any user program's resume
  `JMP` still target `SHELL_PROMPT` itself and now get the leading
  newline they need. Covered by `Editor.RunResumeLandsBackInTheShellNotRawWozmon`
  and both Ctrl-C tests continuing to pass against the `>`-prompt
  assertions above (a regression here would show as the reprompt glued to
  the preceding output again).
- **`NEW` and `SAVE` now print a shared `Ok` instead of `NEW`/`OK`.** A
  pre-existing `MSG_OK` (`"OK"`, all-caps) already existed for `ASM`'s
  clean-assemble confirmation; rather than add a second, differently-cased
  message just for `NEW`/`SAVE`, the single shared constant's text changed
  to `"Ok"` and `DO_NEW` (previously its own `MSG_NEW: "NEW"`) and
  `DO_SAVE` (previously silent — no completion message at all) both now
  reference it — one consistent success string across all three
  confirmations instead of three different ones. Every existing test
  checking for the old text (`"OK"` after `ASM`, `"NEW"` after `NEW`) was
  updated to `"Ok"`; the `NEW`-after-`NEW` GoogleTest case in particular
  had been incidentally passing for the wrong reason before this pass —
  `out.find("NEW")` was matching `READLINE_ECHO`'s own echo of the typed
  command word "NEW", not the completion banner it was meant to check —
  so it now checks for `"Ok"` instead, actually exercising the banner.
  One `saveload.spec.ts` test needed more care than a literal swap: it
  types `SAVE` before `LOAD`, and since both now print the identical `Ok`,
  a bare `raw.includes("Ok")` synchronization wait would resolve
  immediately against `SAVE`'s own `Ok` rather than actually waiting for
  the subsequent `LOAD` — fixed by checkpointing the raw output spy's
  length right before triggering `LOAD` and polling only the slice after
  it, the same "don't reuse a signal a prior step in the same test already
  produced" lesson `saveload.spec.ts` already had to learn once for
  "STA $50" (see the Help-panel/pollution tests above).

**A real bug: backspace silently stored an invisible garbage byte instead
of erasing anything — the actual cause of the reported "ghost lines."**
Reported as "I keep getting ghost lines in the program because it takes
input no matter what." Two separate problems turned out to be layered on
top of each other:

- **Wrong byte, not just a missing feature.** `READLINE_ECHO` only ever
  recognized `$08` (BS) as "erase the previous character." But the
  physical Backspace key on any keyboard, typed through this board's own
  browser terminal (`app.js`, xterm.js), sends `$7F` (DEL) — confirmed
  directly by spying on `Machine.typeChar` in the running browser and
  watching the actual byte cross into the emulated ACIA. `$08` is a real
  byte too (what a genuine ASR-33/Teletype-style keyboard's own Backspace
  key sends), just not the one the deployed UI ever produces. Since
  `READLINE_ECHO` didn't recognize `$7F` at all, every real backspace
  press in the browser fell through to the ordinary-character path and
  got silently written straight into `LINE_BUF` as if it were typed text
  — an invisible control byte embedded mid-line, with no visible sign on
  screen anything was wrong (DEL doesn't render as a glyph). That
  embedded byte then rides along through `STORE_LINE` into the actual
  program buffer: a genuine data-corruption bug, not a cosmetic one —
  "ghost lines" because the stored line silently differs from what it
  visually looks like on screen, not because of a display artifact.
  Fixed by recognizing both `$08` and `$7F` as the same "erase" trigger.
- **Even the recognized case wasn't destructive.** `READCHAR` (bios.s)
  echoes every raw byte it reads unconditionally, `$08` included — but a
  bare BS only moves a terminal's cursor left one column; it doesn't
  erase the glyph already there. So even a real ASR-33-style `$08`
  keypress left the old character's glyph lingering on screen under
  whatever got typed next — most visible when retyping a shorter line
  over a longer one, which reads exactly like a "ghost" second entry even
  though the underlying buffer was already correct in that specific case.
  Real teletype-era hardware with no addressable erase genuinely worked
  this way — `wozmon.s`'s own `BACKSPACE` (`DEY`/`BMI GETLINE`) is exactly
  this non-destructive convention, period-accurate there for the real
  Apple 1 monitor it recreates. But this shell is original software for
  this board, not a recreation of a specific historical monitor, so
  nothing here obligates preserving that limitation — fixed with the
  classic destructive-backspace idiom instead (`rl_erase`, `editor.s`):
  overwrite the vacated column with a space, then back up over the space
  too, landing the cursor exactly where the bare erase left it but with
  the glyph actually gone.

The two bytes need different handling in that shared `rl_erase` step,
not identical treatment: `$08`'s own automatic echo already moved the
cursor left by the time `READLINE_ECHO` gets to classify it, but `$7F`
in the *output* direction is the classic teletype "rubout, ignore me"
byte — a real terminal (xterm.js included) treats it as a non-printing
no-op, so it never moves the cursor on its own. The `$7F` path has to
supply that missing leading BS itself before calling `rl_erase`, or the
erase would land one column too far right. Getting this wrong is exactly
the kind of thing a naive "just add `$7F` as another way to trigger the
same code" fix would silently get wrong in the browser while still
passing a GoogleTest suite that only ever exercised `$08`.

Covered by `Editor.BackspaceErasesTheCharacterNotJustTheCursor` (`$08`),
`Editor.DelAlsoErasesDestructivelyAndDoesNotCorruptTheStoredLine` (`$7F`
— asserts both the exact echoed byte sequence *and*, more importantly,
that the corrected line stores and lists with no stray `$7F` leaked into
it, directly proving the corruption case is fixed, not just the cosmetic
one), and `Editor.BackspaceOnAnEmptyLineIsANoOp` (`tests/editor_test.cpp`,
both bytes, Y=0 guard unchanged). `editor.spec.ts`'s "backspace erases
the character, not just the cursor" drives the same scenario through the
real browser terminal using `page.keyboard.press("Backspace")` — the
actual key, not a hand-picked byte — spying on the raw output stream the
same way `saveload.spec.ts` already does, and would have failed against
the pre-fix ROM exactly the way the user's own report described.

**`CHECK_SYNTAX`: bad opcode/operand syntax rejected the instant a line is
typed, not only at `ASM`/`RUN` time.** Requested directly: a way to catch
"garbage entered by the user" — a typo'd mnemonic or malformed operand —
without needing to check whether any label it mentions actually exists
yet. `STORE_LINE` (the single mutation primitive every write path already
funnels through — numbered entry, `EDIT`'s retype, `LOAD`'s per-line
ingestion) now calls a small new `CHECK_SYNTAX` first: it reuses
`PARSE_LINE` itself — the *exact* routine `ASM_PASS1`/`ASM_PASS2` use for
real assembly, not a second, parallel implementation that could drift out
of sync with what `ASM` actually accepts — against the about-to-be-stored
text. If `PARSE_LINE` sets `ERRFLAG` (unrecognized mnemonic, malformed
`$xx`/`#$xx` operand, an unsupported addressing mode for that mnemonic,
…), `STORE_LINE` prints `?SYNTAX` and bails out before touching the buffer
at all — nothing is deleted or inserted, so a bad retype can't clobber a
good existing line, and a bad auto-numbered line doesn't silently consume
a line number either. This is a real, well-documented precedent, not
invented for this board: classic Microsoft BASIC (and its derivatives —
this repo's own now-retired `msbasic_galloac` among them) tokenized and
syntax-checked each line as it was typed, printing `?SYNTAX ERROR`
immediately rather than waiting for `RUN` — the same terse `?`-prefixed
shape this ROM's own pre-existing `?LINE` (`MSG_BADLINE`) already uses,
so `?SYNTAX` (`MSG_SYNTAX`) fits the established house style exactly.

**Deliberately label-agnostic — this is a syntax check, not an early
resolve.** `PARSE_LINE`'s mnemonic lookup (`MNEM_LOOKUP`) and addressing-
mode resolution (`RESOLVE_MODE`/`RESOLVE_SIZE`) never consult the symbol
table — only `SYM_DEFINE` (pass 1) and `EMIT_INSTRUCTION`'s operand
resolution (pass 2) do, and neither runs here. A labelled operand
(`PARSE_OPERAND`'s `@label` path) is accepted purely structurally (looks
like an identifier), its value never looked up — so `20 JMP FORWARD`
typed before line 30 ever defines `FORWARD` passes cleanly, exactly like
a real two-pass assembler accepts a forward reference; `ASM`'s own passes
remain the only place an *undefined* label is ever actually caught
(`LOOKUP_LABEL`, pass 2). Checking labels here would have meant rejecting
every forward reference a normal, sensibly-ordered program makes routine
use of — a real design line worth drawing precisely, not just a
convenient scope cut.

**Comments have to be excluded from the checked range, or a perfectly
ordinary trailing comment reads as bad syntax.** The text `STORE_LINE`
receives is whatever the user actually typed, comments and all (`PRINT_ENTRY`
needs that same raw text for `LIST`/`EDIT` — see above), but
`PARSE_OPERAND`'s label path rejects a bare `;` outright (`IS_ALPHA` fails
on it → `@badoperand`) — so `NOP ; nothing to do` would false-positive as
`?SYNTAX` without this. `CHECK_SYNTAX` finds the first `;` in the checked
range (the same trigger `ASM_READLINE`'s own comment strip already uses)
and temporarily shortens `LINELEN` to stop right there before calling
`PARSE_LINE`, restoring the real length immediately after — `PARSE_LINE`
only ever *reads* `LINE_BUF`, never writes it, so this is safe, and no
second scratch buffer/copy is needed. This also meant `PARSE_LINE` itself
had to change: it used to unconditionally reset `CURSOR` to 0 at its own
top, which is fine for `ASM_PASS1`/`2` (their `ASM_READLINE`-sourced lines
always start at offset 0) but wrong for `CHECK_SYNTAX`, whose checkable
text starts at `RESTOFF` (nonzero for an explicitly-numbered line, past
the `"10 "` prefix). `PARSE_LINE` now expects the caller to set `CURSOR`
first; both real call sites picked up their own explicit `stz CURSOR`.

Covered by `Editor.RejectsBadSyntaxImmediatelyAtEntryWithoutCheckingLabels`
(`tests/editor_test.cpp`) — a bad mnemonic rejected and never stored, a
forward-referencing `JMP` to a not-yet-defined label accepted and stored
correctly. Fixing this exposed several *pre-existing* test fixtures that
had been relying on the old unchecked-entry behavior to store text that
was never valid 6502 in the first place — `AAAA…`-style filler used only
to exhaust the source buffer (now a comment-only line, `; AAAA…`, so
`CHECK_SYNTAX` skips it exactly like `ASM` itself would), a bare `RUN`
used to prove `LOADMODE` prevents command-hijacking during `LOAD` (now
also correctly `?SYNTAX`-rejected as a second, independent safety net —
`Load.LoadDoesNotLetAnUnnumberedLineThatReadsLikeACommandHijackTheTransfer`
now asserts the transfer continues normally around the rejected line,
auto-numbering the next real line 20, not 30, since the rejected one
never consumed a number), a decimal `JMP 10` operand (always invalid —
this assembler takes hex operands only, `$xx`) in a test that only cared
about `LIST` sorting by line number, and an unrecognized-mnemonic test
that was checking `ASM`'s own `"ERR LINE n"` for exactly the class of
error that's now caught earlier and never reaches `ASM` at all
(`Assembler.RejectsAnUnknownMnemonicImmediatelyAtEntryNotJustAtAsm`,
renamed to match).

### 12.x `SHELL_START` banner no longer runs into Wozmon's own examine echo

Typing `8000R` at Wozmon's real prompt to enter the shell hits its
genuine "AAAA R" **examine-then-run** syntax: real Wozmon echoes
`"8000: <byte>"` (its own EXAMINE-mode echo, `wozmon.s`'s `NOTCR`/`ECHO`)
*before* the `JMP`, with no trailing CR/LF of its own — authentic 1976
Apple-1 behavior, not a bug (see the "4C" question this raised — that's
just the actual opcode byte stored at $8000). Without a newline of its
own first, `SHELL_START`'s banner ran right onto the same row as that
echo: `"8000: 4C6502 ASSEMBLY CODER"`. `SHELL_START` now prints CR/LF
before `MSG_SHELL`, landing the banner cleanly on its own line regardless
of how the shell was entered. Verified by the existing 83/83 GoogleTest
pass (no new test added — this is pure cosmetic terminal-row placement
with no new branch/state to assert beyond what boot-into-shell tests
already exercise).

### 12.z SAVE's own dump overwrote itself on screen (display-only fix)

`SAVE`'s wire format is deliberately bare-CR-separated between lines (no
LF — see load.s's own header, matching the same "<num> <text>\r" shape
`LOAD` parses back), which is correct for the actual transfer and for
anything genuinely listening on the wire. But displayed raw, a bare CR
just returns a real terminal's cursor to column 0 without advancing a
row — each line overwrites the previous one in place, and the trailing
`Ok` lands on top of whatever's left of the last line instead of getting
its own row (reported as `>SAVE` / `Ok JMP $80FA` / `>` — only the last
program line and `Ok` survived visibly, overlapped). This is the same
overwrite artifact `DO_LOAD`'s own incoming-echo already works around on
the ROM side (its header comment) — the SAVE direction just never got the
equivalent fix.

Fixed entirely client-side (`app.js`'s `pullSerial()`), not in the ROM:
the actual bytes transmitted stay byte-exact (so the file format, LOAD's
round-trip, and anything really listening on the ACIA see the real, plain
wire content) — only `outQ` (this page's own on-screen draw queue) gets a
synthetic LF appended whenever a CR isn't immediately followed by a real
one, tracked via a `pendingLf` flag that persists across animation
frames (so it's correct regardless of where a chunk boundary falls
relative to a CR/LF pair). `saveCapture` and the banner-detection logic
both still see the untouched original byte.

Covered by `tests/saveload.spec.ts`'s "typed SAVE prints each line on its
own row..." test, which reads xterm's own line buffer directly
(`window.__term.buffer.active`, newly exposed alongside the existing
`window.__machine`) rather than scraping `#screen`'s flattened text, since
row placement is exactly what's under test.

### 12.aa Board graphic cleanup, Power toggle, and the landing-page thumbnail

Reference designators (`U1`/`U2`/.../`J1`/.../`SW1`, etc.) were visible
silkscreen text on the board SVG (`gen_pcb_svg3.py`'s original output,
now hand-maintained inline in `index.html` — see that generator's own
header comment; the script itself wasn't preserved from whatever session
produced it). Stripped them in favor of function labels a visitor
actually wants: `6502 CPU` (was `65C02`, ref `U1` dropped), `ROM`/`RAM`
each gained a `32K` line (both are genuinely 32K parts — `62256` SRAM,
`28C256` EEPROM, confirmed against `cpu6502/pcb/pcb6502/FullBoard/
pcb6502.kicad_pcb`'s own `Value` properties, not guessed), `REG` (was
`U6 REG`), crystal labels simplified to bare `1 MHz` / `1.8432 MHz`. The
three LEDs (`D1`/`D4`/`D7`) are now `Pwr`/`Rx`/`Tx` — their actual
function, matching `pcbLedD1`/`D4`/`D7`'s own `led-power`/`led-rx`/
`led-tx` classes. `.lbl-chip-ref` (the small ref-designator text style)
is renamed `.lbl-chip-sub`, now used only for the `32K` sub-labels.

**RST**: the pushbutton (`SW1`) is the real click target
(`app.js`'s `[data-ref="SW1"]` listener, unchanged) — it previously had
*two* separate, confusing labels (`U9 RST` positioned near U9, the
DS1813 reset-supervisor chip, some distance from the actual button; a
bare `SW1` beside the real button) — `U9 RST` is gone, `SW1`'s text is
now `RST`, right by the actual clickable part. Also added a padded
invisible `.pcb-hit` rect inside SW1's group, well beyond the visible
button housing, so the click target is more forgiving than the tightly-
drawn silkscreen footprint alone.

**Power (J1)**: the barrel jack graphic is now clickable, toggling a new
`poweredOn` flag — `frame()`'s main loop skips CPU/serial/LCD work
entirely while off (CPU genuinely frozen, not just visually paused;
verified by `cycleCount()` staying flat across a wait in the new
Playwright test), keystrokes are ignored, and D1 dims (`led-power` class
removed). This has **no real-hardware equivalent** — the actual board
has no power switch, J1's just a jack, always live when plugged in — so
it's flagged in both `app.js` and the Help/board-panel prose as a
labelled UI convenience, not a hardware claim. It pauses in place rather
than resetting, specifically so a program mid-edit survives a "power
cycle" — closer to "the monitor's unplugged" than "the machine lost its
memory."

**J2 (DB9)** is now labelled `Serial`.

**J7 (interrupt routing) and J5 (BOOT select)** rows removed from below
the board entirely, along with their `<select>`/checkbox controls and
`app.js`'s now-orphaned listeners (`setViaIrqRoute`/`setAciaIrqRoute`/
`setBootSelect` calls) — `bus.h`'s own `JumperState` struct defaults
(`via_irq_route = Irq`, `acia_irq_route = Nmi`, `boot_select = 0`) already
match the shipped ROM's wiring with zero JS needed, confirmed by reading
those defaults directly rather than assuming. J8 (RTS→CTS) keeps its
overlay checkbox — it's a real, useful toggle with no default that "just
matches the ROM," unlike J7/J5.

**Landing-page thumbnail**: `retroweb/assets/cg-oac-6502-board.jpg` (a
static screenshot) is replaced by `cg-oac-6502-board.svg`, extracted from
this same cleaned-up board markup with its own self-contained `<style>`
(inline SVG embedded in an HTML document tolerates things strict XML
doesn't — e.g. `--` inside an HTML comment — so the extraction also strips
the one HTML comment that would otherwise fail `<img src=*.svg>`'s strict
XML parse) and a widened, letterboxed `viewBox` (light-gray backdrop)
matching the landing page's other two card thumbnails' 286×128 aspect
ratio, so the row of three stays visually consistent. Verified by
rendering it standalone (`qlmanage -t`) and inspecting the output — real
image content, not just "no parse error" — since a class name typo or a
mispositioned label wouldn't show up any other way.

### 12.ab Board graphic follow-up: a real pin/body mismatch, and more polish

A follow-up pass, caught mostly by re-rendering and zooming into the
board graphic (same `qlmanage -t` technique as 12.aa) rather than by
inspection alone — several of these were only visible at pixel level.

**U2 (ROM)'s pins actually overshot its own body** — a real bug, not a
label issue: the body rect was only 17.74 units tall, but its 14-pins-a-
side rows (real 28C256 spacing) span ±16.51, badly overshooting both top
and bottom (confirmed by diffing every chip's body bounds against its own
pin range — U2 was the only mismatch; every other chip's pins sit
correctly inside its body with the same ~1.6-unit margin). The instinct
fix — enlarge the body to match — turns out to collide with RAM (U3),
which sits at an equally real, KiCad-traced position directly below with
no slack: there's genuinely no room in this diagram's layout for a
literal 28-pin-DIP-sized ROM body. Fixed the other direction instead:
kept the real pin count (14/side) and the original, correctly-laid-out
body size, and compressed the pin spacing (~0.44×) to fit inside it —
same visual "this is a 28-pin chip" impression, no more overhang, no
layout collision.

**LED labels** (`Rx`/`Tx`/`Pwr`) moved above their LEDs instead of below.

**`REG`** label removed (kept the chip shape, just no text — matching
how the small jumper headers went label-less in 12.aa).

**J2 relabeled `RS232`** (was `Serial`), and **Y1's crystal** gained a
purpose label alongside its frequency — `RS232 1.8432 MHz` instead of a
bare `1.8432 MHz` that didn't say what it clocked (it's the ACIA's baud-
rate crystal).

**J8 (RTS→CTS) overlay checkbox removed** — like J7/J5 in 12.aa, it now
just defaults to the shipped ROM's own wiring (`rts_to_rs232_cts = false`
in `bus.h`, "no trace populates this, it's jumper wire only" — genuinely
never asserted by anything in this design). The now-fully-unused
`.pcb-overlay` CSS block (only ever styled J8's div) is removed too, not
just the one div — dead code, not future-proofing.

**RST's red button cap is properly clickable**: `.sw-button` (the circle)
didn't have `cursor: pointer` — `.sw-body` (the housing rect underneath)
did — so hovering the part that visually reads as "the button" gave no
click affordance even though the click handler (bound to the parent `<g>`,
covering the whole group via event bubbling) already fired correctly
either way. A cursor-affordance bug, not a functional one.
