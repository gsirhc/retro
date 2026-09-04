# Altair 8800 — Independent Fidelity Review

**Date:** 2026-09-04
**Scope:** `retroweb/altair8800/` — C++ 8080 core, peripheral boards, WebAssembly
wrapper, browser front end, both test suites.
**Reviewer brief:** historical accuracy (accepting that some choices were made to
let more software run), preset accuracy, code architecture, strict 8080
execution fidelity, hardware/bus accuracy including I/O traps for the disk
controller / paper tape reader / cassette, serial terminal accuracy, and whether
the unit + E2E suites are adequate.

**Nothing in the repo was changed by this review.** Every item below is a
finding, a question, or a work item — with the owner's decisions already
recorded where they were given (see *Owner decision* lines).

This file now lives at `retroweb/altair8800/ALTAIR_REVIEW.md` (moved from the
repo root) and is versioned as the source of truth for review findings and
responses — see the response log immediately below for what happened to each
item, added the same day as the review.

---

## Response log — 2026-09-04, implementing agent

Verified the review's factual claims against source before acting on any of
them (EI/interrupt dead-code, Kill-the-Bit OR-not-brightness, the RAM/ROM read
order, the `test` substring guard, the cassette-tick/runCycles coupling) — all
confirmed exactly as written. Where a response below disagrees, it's about
scope or sequencing, not a correction of the review's facts.

| # | Item | Status |
|---|---|---|
| §5.4 | "~716 bytes free" | **Resolved empirically** — booted `stock` with Auto-load and read the ROM's own banner. It prints `716 BYTES FREE`, and `4K BASIC 4.0 / COPYRIGHT MITS 1976` — which also answers §5.3's version-number question: not invented, it's what the ROM itself prints. No doc changes needed. |
| §7.1 | CLAUDE.md's track-0 example | **Fixed.** Replaced with the `ANA` auxiliary-carry quirk as the "keep a real hardware quirk" example, and stated the SIMH-artifact-vs-hardware-quirk distinction explicitly so it doesn't drift back. |
| §3.5 | Boot PROM writable at 64 KB | **Fixed.** `bus.read`/`bus.write` now check the ROM window before the RAM ceiling, and `bus.write` drops writes into it. New disk.spec.ts test drives an `STA` through the CPU (not `writeByte()`, which bypasses the bus) to prove it. |
| §6.8 | Test-seam guard | **Fixed** — real `URLSearchParams` parse instead of `.includes("test")`. |
| §6.7 | `make cpm` not in `make check` | **Fixed** — `check: demo test cpm`; no-ops harmlessly if `cpm/*.COM` aren't fetched locally. |
| §5.1 | Preset metadata (part numbers, eras, RAM story, blurb contradiction) | **Fixed** — see §5.2 response for the era reasoning. Real MITS part numbers (88-4MCS/88-4MCD/88-16MCD ×4), CP/M 2.2 → 1979, the 64 KB RAM story now told one way (4× 88-16MCD, matching the disk-cabinet hint), `baremetal`'s blurb no longer contradicts its own card list. Pinned in `presets.spec.ts`. |
| §7.2 | `printManual` speed override | **Fixed** — one-line carve-out added to CLAUDE.md's overrides list. Also corrected the `?test=1` clause's stale "floppy load speed" claim while in there (§3.2d is still unimplemented — see below). |
| §2.4 | `Cpu::reset()` clears all registers | **Doc comment only, as the review suggested** — not building a "leave registers alone" mode. Real MITS software already can't rely on post-reset register state either way; a mode that injects garbage would only matter for adversarially testing software's own init discipline, not a goal here. |
| §3.6b | Kill the Bit lamps: OR, not brightness | **Fixed.** `addr_or_` replaced with a 16-element per-bit hit-count array (`Machine::busActivityCounts()`), mapped to lamp opacity in `app.js`. `panel-lights.spec.ts` now asserts two simultaneously-lit high bits render at different brightness, sampled every rendered frame in-page (Playwright's own poll round-trip was too slow to reliably catch the ~16 ms transition window — see the test's comment). |
| §4 | Terminal profiles are cosmetic only | **Fixed, phased as A/B/C rather than one PR** (see the plan in the conversation this ran in): ASR-33/Glass TTY strip all escape sequences (a stateful filter, tested for a sequence split across filter calls too); VT52 translates the real DEC VT52 set to xterm-compatible sequences; ADM-3A translates its bare control-code cursor moves and `ESC =` addressing, validated by booting the real WordStar disk under the profile (renders cleanly, no corruption — though WordStar's sign-on screen hadn't yet had ADM-3A selected in its own menu at the point checked, so its specific codes weren't exercised live; the translator is unit-tested against the spec directly). ASR-33 now forces CAPS LOCK on. Baud-metering tests added for the five previously-untested profiles. VT100/Modern correctly have no filter (they're genuinely ANSI-compatible). New `termfilters.spec.ts`. |
| §5.2 | 88-SIO vs 88-2SIO | **Option 1 (relabel), by agreement.** `baremetal`/`stock` moved to 1976 (when the 2SIO existed) rather than their software's earlier real MITS release date; documented as a deliberate choice in `app.js`, the web README, and here. Option 2 (implement 88-SIO) stays undone — see "Deferred" below for why it's coupled to §5.3's bootstrap question. |
| §3.2b | `Disk88::reset()` homes the head | **Fixed.** RESET now only deselects the controller and clears its latches (`selected_`, `flags_`, `sector_`, `bufpos_`, `write_dirty_`); the head-homing loop is gone. New GoogleTest `DiskTest.ResetDoesNotHomeTheHead`, plus a Playwright test that boots CP/M (which seeks off track 0), hits RESET, and confirms the track is unchanged while the controller does deselect. |
| §3.4a/b | Cassette transport frozen while stopped; RESET rewinds the tape | **Fixed.** `Machine::reboot()` no longer calls `cassette_.reset()` at all (the deck isn't on the bus RESET reaches). The frozen-transport bug needed more than removing a call, though: `cassette_.tick()` was paced entirely by `cpu_.cycles`, which never advances while the CPU is stopped. New `Machine::tickCassette(dtMs, running)`, called every rendered frame regardless of run state: while running it still paces off `cpu_.cycles` exactly as before (so 25×/50× and turbo stay correct); while not, it advances a separate `idle_cycles_` counter by real elapsed time at 2 MHz-equivalent, added to `cpu_.cycles` before the same `cassette_.tick()` call — so the two paces splice with no jump when RUN resumes. Two new Playwright tests: PLAY keeps winding with the CPU on STOP, and RESET touches neither tape position nor transport state. |
| §2.2, §2.3 | `EI` delay, interrupt wiring | **Fixed.** `Cpu::ei_delay_` blocks `interrupt()` for exactly one retiring instruction after `EI` (counted down at the top of `step()`, not decremented by HALT's own idle cycles). `sio_.on_irq` now fires `cpu_.interrupt(0xFF)` (RST 7, the conventional Altair 2SIO vector) in `Machine`'s constructor. New `tests/interrupt_test.cpp` (8 cases): DI-ignored, EI-taken, HALT-wakes, HALT-stays-halted-while-disabled, INTE-cleared-on-accept, the vector for all 8 RST n, the EI delay itself, and EI-immediately-followed-by-HLT. Full Playwright suite re-run afterward specifically to check no bundled software's dormant interrupt-enable now misbehaves — none does (nothing in `disks/`/`roms/` was found to exercise this path; it's correct but, as flagged below, still unproven against real software). |
| §3.2a | 138th byte per sector | **Fixed** — `in(0x0A)`'s fill-buffer bound corrected from `kSectorLen + 1` to `kSectorLen`, so a 138th read re-delivers the sector from byte 0 (SIMH parity) instead of a phantom always-zero byte. New GoogleTest. |
| §3.2c | Empty drive reports healthy | **Fixed** — `OUT 0x08` (select) no longer sets the SIMH-0x1A "enabled" bits for an unmounted drive; `OUT 0x09`'s `FN_HEAD_LOAD` no longer sets `F_HEAD`/`F_NRDA` for one either. An empty, selected drive now reads all-false, and a boot/read attempt against it gets a clean "not ready" rather than a fabricated all-zero sector. Two new GoogleTests (one replaces a test that had encoded the old, buggy expectation). |
| §3.2d | 88-DCDD has no rotation timing | **Fixed** — `Disk88::tick()`/`setSpeed()` gate `IN 0x09`'s sector advance on a sectors/sec credit, same shape as `CassetteACR`'s byte credit; default (never called) is unlimited, so every pre-existing caller (native harnesses, GoogleTest) is unaffected. Browser build adds a LOAD SPEED selector to the disk cabinet (Realistic ≈193/sec ≈166 ms/rev / 5×/25×/50×/Max-under-`?test=1`), matching the paper-tape/cassette pattern exactly. New fidelity test proves Realistic measurably slows a CP/M boot. |
| §3.6a | Status lamps (MEMR/M1/etc.) from real machine-cycle status | **Partially fixed — see the note below on what's still approximated.** `MEMR`/`WO`/`INP`/`OUT` now reflect which `Bus` callback fired last (tracked in `wasm_machine.cpp`, no CPU-core change needed since `read`/`write`/`in`/`out` were already four separate callbacks); `INT` lights when `cpu_.interrupt()` was actually served since the last poll. `HLDA`/`PROT` were already correct as permanently off (no DMA peripheral; PROTECT is a documented no-op) — not bugs, just mislabelled as such in the original finding. `M1`/`STACK` remain approximated; see below for why. |

### What's still approximated in §3.6a, and why

`M1` (opcode fetch vs. a plain operand read) and `STACK` (a push/pop vs. any
other memory access) need the CPU core itself to say what an access is *for* —
the `Bus`'s four callbacks say *what kind* of access (read/write/in/out) but
not *why*. That's a real decoder-level change (tagging every one of `i8080.cpp`'s
opcode handlers with its access's purpose), touching the one part of this
codebase proven correct by 8080EXM, for a payoff that's arguably invisible
regardless: both are sub-frame pulses (a handful of T-states within a ~10-18
T-state instruction) and the front panel is polled once a rendered frame —
the Kill-the-Bit brightness fix worked by aggregating *counts* over many
cycles, but a boolean status bit has no equivalent trick. Recommend leaving
this as the one deliberately-scoped-out remainder of §3.6a rather than take
on the core-decoder risk for it.

### Deferred, by explicit decision

- **§5.2, 88-SIO — deferred, not scheduled.** Staying with Option 1 (relabel,
  already applied): `baremetal`/`stock` dated 1976, the false 88-2SIO-in-1975
  anachronism gone, the simplification documented in `app.js`/the web
  README/here. Option 2 (implement an actual 88-SIO board at ports 0x00/0x01)
  is explicitly not being done — this is a real feature (a new peripheral,
  plus sourcing and pinning a period SIO-targeted BASIC image that may not
  exist in fetchable form), not a fidelity nit, and doesn't earn its cost
  against everything else in this pass. It stays coupled to §5.3 (below) if
  it's ever picked up.
- **§5.3, the deramp.com 3.2 bootstrap — deferred with it.** The
  `LDR4K32-octal.PRN` loader already sourced (`altairclone.com/downloads/
  4kbootstrap.pdf`, read in full earlier in this same review's conversation)
  runs on **ports 0x00/0x01 — actual 88-SIO**, not this emulator's 2SIO.
  Teaching it in the panel guide only makes sense once/if §5.2's option 2 is
  taken — porting the same loader to 2SIO ports would stop being "an actual
  artifact" and defeat the reason for wanting it. Decide both together,
  later, as one question, not two separate backlog items.
- **§2.2/§2.3, interrupts are wired but functionally unproven.** Fixed and
  unit-tested (above), and the full Playwright suite confirms nothing bundled
  breaks -- but that's a negative result, not a positive one: nothing in
  `disks/`/`roms/` was found to actually *use* RST-vectored interrupts, so
  the wiring has never been exercised by real period software end to end. If
  this matters going forward, the honest next step is a tiny purpose-built
  test ROM that enables interrupts, arms the 2SIO's RX-ready IRQ, and proves
  a keystroke actually vectors to 0x38 -- not folded into this pass.
- **§8, `app.js` module split:** the review itself says not urgent. Agreed —
  2,700+ lines in one closure with 200+ tests pinned against its exact
  behavior is a large-regression-surface refactor for zero user-facing
  benefit. Left alone.

All work above was verified against the actual test suites before being
called done: 213/213 Playwright tests, 66/66 GoogleTests (including the new
`interrupt_test` suite), and all four CP/M diagnostics
(8080PRE/TST8080/CPUTEST/8080EXM) green after every change, not
just at the end.

---

## 0. How this was verified

Not read-only guesswork. What was actually executed:

| Check | Result |
|---|---|
| `make -C retroweb/altair8800 test` | **55/55 pass** — Arith 27, Cassette 20, DiskTest 8 |
| `make -C retroweb/altair8800 cpm` | **8080PRE, TST8080, CPUTEST, 8080EXM all pass** |
| 8080EXM CRC check | All 25 groups PASS with matching CRCs |
| `kCycles[256]` vs. Intel 8080 datasheet | Hand-verified entry by entry — **no errors found** |
| Ad-hoc C++ harness against `disk88.cpp` | Confirmed three edge behaviours (§3.2) |

The 8080EXM pass is the single most important result in this review: it
exercises every documented flag combination, including auxiliary carry and the
PSW's fixed bits 1/3/5, and it matches. The ALU and flag model is correct — this
is not an inference.

---

## 1. Executive summary

**What is genuinely excellent:**

- The CPU core passes the exhaustive 8080 instruction exerciser with correct CRCs.
- The cycle table is correct in all 256 entries.
- The 88-ACR cassette is the best-modelled device in the project — linear tape
  with a real head position, record-at-head with overwrite, inter-program gaps,
  transport auto-stop, credit-based throttling, and status polarity derived from
  and cited against the actual 8K BASIC ROM.
- CI boots a real, pinned MITS CP/M 2.2 diskette on the emulated 88-DCDD and
  runs `DIR`/`STAT`, and blocks the deploy on failure.
- Provenance is cited in file headers (SIMH `altair_dsk.c` / `altair_cpu.c`, the
  ROM-derived cassette polarity) exactly as CLAUDE.md requires.
- Architecture is clean: a host-agnostic CPU behind a `Bus` struct, peripherals
  as independent `owns()/in()/out()` units, an embind wrapper that composes them.
  The same `Disk88` runs under the native `cpm_disk` harness and in the browser.

**What needs attention, in priority order:**

1. Serial terminal profiles are cosmetic only — no actual terminal emulation
   differences. **Owner wants these 100% realistic.** (§4)
2. Panel RESET rewinds the cassette and homes the disk heads. **Owner confirms
   this should be realistic.** (§3.4)
3. The interrupt path is entirely dead code, and `EI` has no one-instruction
   delay. **Owner wants this wired up.** (§2.2, §2.3)
4. Kill the Bit's address lamps show an OR, not brightness — the marquee visual
   of the bare-metal preset doesn't work. **Owner independently noticed this.** (§3.6)
5. The 88-DCDD has no rotation or step timing at all — the one unlabelled speed
   departure in the project. (§3.2)
6. CLAUDE.md and the code contradict each other on the SIMH track-0 latch. (§7.1)
7. Preset era/hardware anachronisms, and an unverified BASIC version number. (§5)
8. Test gaps: no `serial2sio` unit test, no T-state assertions, five of seven
   terminal baud rates unmetered. (§6)

---

## 2. Intel 8080 execution fidelity

### 2.1 Verdict: strong, empirically proven

`8080EXM.COM` passes with matching CRCs across all 25 groups:

```
dad <b,d,h,sp>................  PASS! crc is:14474ba6
aluop nn......................  PASS! crc is:9e922f9e
aluop <b,c,d,e,h,l,m,a>.......  PASS! crc is:cf762c86
<daa,cma,stc,cmc>.............  PASS! crc is:bb3f030c
... (all 25 groups PASS)
```

Source inspection agrees with the empirical result:

- **`sub`/`cmp` auxiliary carry** — `retroweb/altair8800/i8080.cpp:59` uses
  `~(a ^ v ^ r) & 0x10`, i.e. the *inverted borrow*. This is 8080 semantics (the
  half-carry out of the internal `A + ~v + 1`), not Z80 semantics. Correct, and
  correct with borrow-in too.
- **`ana` AC quirk** — `i8080.cpp:74` sets AC from `(a | v) & 0x08`, the 8080
  behaviour, not the 8085's always-1. Correct.
- **`dcr` AC** — `(v & 0xF) != 0`, matching `v + 0xFF`'s half-carry. Correct.
- **`daa`** — `i8080.cpp:112` uses `a > 0x99` for the high-nibble correction.
  Worked through the boundary cases (`0x9A`, `0xFA`, AC-set-with-low-nibble ≤ 9,
  `0x99` with AC): exactly equivalent to the Intel manual's "if the most
  significant 4 bits are **now** greater than 9" wording, including carry-out
  from step 1. Carry is never cleared by DAA. Correct.
- **PSW masking** — `PUSH PSW` writes `(f & 0xD7) | FLAG_N1`; `POP PSW` reads it
  back the same way. Bits 3/5 read 0, bit 1 reads 1. Correct, and validated by
  8080EXM (which uses PUSH/POP PSW to accumulate its CRCs).
- **Undocumented opcodes** alias correctly with correct cycle counts:
  `0x08/10/18/20/28/30/38` NOP (4), `0xCB` JMP (10), `0xD9` RET (10),
  `0xDD/ED/FD` CALL (17).
- **Rotates** touch only carry. `CMA` touches nothing. `STC`/`CMC` touch only
  carry. `INR`/`DCR` preserve carry. `DAD` touches only carry. All correct.

**Cycle table (`i8080.cpp:11-28`) — verified entry by entry against the Intel
8080 datasheet.** Spot points worth recording: HLT = 7, XTHL = 18,
SHLD/LHLD = 16, STA/LDA = 13, MVI M / INR M / DCR M = 10, conditional RET 5
(not taken) / 11 (taken), conditional CALL 11 / 17, PCHL = 5, SPHL = 5,
XCHG = 5, EI/DI = 4, IN/OUT = 10. **No errors found.**

### 2.2 Finding — `EI` takes effect immediately

`retroweb/altair8800/i8080.cpp:411`

```cpp
case 0xFB: int_enabled = true; break;          // EI
```

A real 8080 does not recognise an interrupt until **after the instruction
following `EI`**. This is a documented and load-bearing behaviour for the
`EI / RET` idiom in interrupt handlers. Not implemented, not tested.

> **Owner decision (#4):** wire this up along with the interrupt path.

**Work item:** add a one-instruction interrupt-enable delay (a
`pending_ei` flag consumed at the top of the next `step()`), plus a unit test
that proves an interrupt requested immediately after `EI` is not taken until the
following instruction retires.

### 2.3 Finding — the entire interrupt path is dead code

`retroweb/altair8800/i8080.cpp:213` (`Cpu::interrupt`)
`retroweb/altair8800/serial2sio.h:61` (`on_irq`)
`retroweb/altair8800/serial2sio.cpp:47, :91` (fires `on_irq`)

`Serial2SIO` decodes the 6850's receive- and transmit-interrupt enable bits and
fires `on_irq`, but **nothing ever assigns `on_irq`** — not in
`web/wasm_machine.cpp`, not in `main.cpp`. `Cpu::interrupt()` therefore has no
caller anywhere in the repo. The RST jam, the INTE-clear-on-accept, and the
HALT-wake logic are all unreachable and untested.

Note `Cpu::interrupt()` itself looks correct where it can be judged: it returns
0 and leaves `halted` set when interrupts are disabled (a real 8080 in HALT with
DI stays halted until RESET), it pushes the post-`HLT` PC, and it charges 11
T-states for an RST.

> **Owner decision (#4):** yes, wire it up.

**Work item:** wire `sio_.on_irq` to `cpu_.interrupt(0xFF)` (RST 7 — the
conventional Altair 2SIO vector) in `Machine::make_bus()` or the constructor.
Add unit tests for: interrupt ignored while DI; interrupt taken while EI;
interrupt wakes HALT; INTE cleared on accept; PC pushed correctly; the vector
address derived from `opcode & 0x38`.

### 2.4 Minor — `Cpu::reset()` clears every register

`retroweb/altair8800/i8080.cpp:27`

A real 8080 RESET forces only `PC = 0`. `A`, `F`, `BC`, `DE`, `HL`, `SP` are
indeterminate after reset — this is why Altair software always initialises `SP`
explicitly before using the stack. Clearing everything gives the emulated
machine a cleaner post-reset state than a real one, which could mask a bug in
software that forgets to set `SP`.

Low priority; document it or add a "leave registers alone" mode.

### 2.5 Gap — nothing asserts a T-state count

The `fidelity.spec.ts` test *"the 8080 runs at ~2 MHz of emulated time per
wall-clock second"* measures `cycles` delta against wall time. That is
**self-consistent** — it counts what the emulator itself claims — so a typo in
`kCycles[]` would silently change every real-time behaviour in the machine
(BASIC's timing loops, the cassette credit throttle, paper-tape pacing) without
turning a single test red.

**Work item:** add a GoogleTest that walks a table of `{opcode, expected
T-states}` and asserts `cpu.step()`'s return value, covering all 256 opcodes
including the taken/not-taken variants of every conditional CALL and RET.

---

## 3. Hardware, bus, and I/O traps

### 3.1 88-2SIO serial board (ports 0x10–0x13) — correct, one documented departure

`retroweb/altair8800/serial2sio.{h,cpp}`

Correct:
- 6850 status polarity: RDRF active-high on bit 0, TDRE on bit 1.
- DCD/CTS left clear — on a 6850 these status bits read 1 when the input pin is
  *high* (not asserted), so 0 = ready. The header comment gets this right.
- Master reset decoded on counter-divide field `CR1:CR0 == 0b11`.
- `tx_irq_enabled = (value & 0x60) == 0x20` — CR6:CR5 == 01 is RTS-low +
  transmit-interrupt-enable. Correct.
- Reading the receive data register clears the overrun latch. Correct.
- Channel B present at 0x12/0x13, the physical 2SIO layout.
- `owns()` masks `port & 0xFC`, giving exactly 0x10–0x13.

**Documented departure — 512-byte FIFOs** (`serial2sio.h:44`) in place of the
6850's single byte. This is deliberate and explained in the header, but it has a
consequence worth naming explicitly: **a program can transmit 512 characters
before TDRE ever drops.** The claim in `EMU_PRIMER` and CLAUDE.md that "the
Teletype crawls at 10 characters a second and throttles the 8080" is true only
after 512 bytes of slack have been absorbed. For a `LIST` of a long program the
throttle does engage; for anything shorter than 512 characters it never does.

**Minor:** master reset does not flush the RX FIFO, so `RDRF` can survive a
master reset. A real 6850 master reset clears the receiver.

### 3.2 88-DCDD floppy controller (ports 0x08–0x0A) — faithful SIMH port, three confirmed deviations

`retroweb/altair8800/disk88.{h,cpp}`

The port is faithful to Charles E. Owen's `altair_dsk.c`: the `0x1A` enable
flags, the active-low status inversion, the sector register advancing one sector
per read and returning `((sector << 1) & 0x3E) | 0xC0`, the 137-byte sector
buffer, head load/unload, step in/out with clamping, and write-on-`0x80`
followed by a null-filled flush. `disk_bootrom.h` is byte-identical to SIMH's
`bootrom[]`. 337,568-byte images = 77 × 32 × 137 is correct.

Three deviations were **confirmed empirically** with an ad-hoc harness built
against `disk88.cpp`:

**(a) `in(0x0A)` returns 138 bytes per sector, not 137.**
`retroweb/altair8800/disk88.cpp:91` — `if (bufpos_ < kSectorLen + 1)` lets index
137 through, and the fill loop always zeroes that index. Measured against an
image filled with `0xAB`: byte 137 and byte 275 come back `0x00`. SIMH refills
at 137. Harmless in practice (every real BIOS reads exactly 137 bytes then
re-reads the sector register), but it is an emulator-specific artifact — real
hardware would deliver the next physical bytes off the rotating disk, and SIMH
re-delivers the same sector. Untested.

**(b) `reset()` snaps every drive's head to track 0.**
`retroweb/altair8800/disk88.cpp:26` — `for (Drive &d : drives_) d.track = 0;`
Measured: step in twice → track 2 → `reset()` → track 0. A real bus reset
deselects the controller and clears its latches; **it does not move the heads.**
This can mask a BIOS whose head-home routine is broken, which is precisely the
class of bug the track-0 fix in §7.1 was made to expose.

> **Owner decision (#8):** RESET should behave realistically.

**(c) Selecting an empty drive reports a healthy drive.**
`retroweb/altair8800/disk88.cpp:124` — `flags_ = F_MOVE | 0x08 | 0x10` is set
unconditionally on select, without checking whether a diskette is mounted.
Measured: selecting an unmounted drive returns status `0xA5` (= `~0x5A`:
enabled, head-move OK, at track 0) — indistinguishable from a loaded drive.
`in(0x09)` then advances the sector register and `in(0x0A)` returns zeros. On
real hardware an empty drive produces no index pulses, so the sector register
never becomes valid and NRDA never comes true. The practical difference: a boot
attempt on an empty drive fails as a checksum error rather than as "no disk".

**(d) No rotation or step timing whatsoever — the one unlabelled speed departure.**

`disk88.h` states plainly: *"there is no rotation timing model — reading the
sector-position port simply advances to the next sector."* That is justified as
SIMH parity, and it is the right *correctness* choice (it is what the MITS boot
PROM and CP/M BIOS are written against). But it is a *correctness* argument, not
a *speed* argument, and the speed consequence is large:

- Real 88-DCDD: ~166 ms per revolution, 32 sectors → ~5.2 ms/sector, plus ~10 ms
  per track step. A CP/M `DIR` audibly takes a moment; the drive makes noise.
- Here: reads complete as fast as the CPU can poll `IN 0x09`. Effectively instant.

Measured against CLAUDE.md's three rules for overrides:
1. *"the default is always the realistic behavior"* — **fails**, there is no
   realistic mode.
2. *"the override is clearly labelled as a departure"* — partially; it is
   documented in the header but not surfaced in the UI.
3. *"it's opt-in (a control the user chooses)"* — **fails**, there is no control.

Note also that CLAUDE.md's `?test=1` clause says tests may force *"paper-tape /
cassette / **floppy** load speed to `Max`"* — but there is no floppy speed
selector to force, and `app.js`'s test seam only calls
`paperTape.setSpeed("max")` and `cassette.setSpeed("max")`.

> **Owner decision (#2):** report as a potential issue. (No implementation
> decision taken.)

**Options, if it is ever addressed:** add a `LOAD SPEED` selector to the disk
cabinet matching the paper-tape and cassette pattern (Realistic / 5× / 25× / 50×,
Realistic default, internal `Max` for `?test=1`), gating `in(0x09)` and
`in(0x0A)` on a CPU-cycle credit exactly the way `CassetteACR::credit_` does.
Realistic would make a CP/M cold boot take a couple of seconds and a `DIR` a
noticeable beat — which is what the real machine did.

### 3.3 Paper tape — not a bus device at all

There is **no 88-PTR board and no I/O trap for the reader.** `tape.tick()`
(`web/app.js`, the `tape` object around line 2160) pushes bytes into the 88-2SIO
channel-A RX FIFO via `machine.sendByte()` — the same path a keystroke takes.

This is a **defensible and arguably correct** model: on a real Altair the paper
tape reader most people used was the one built into the ASR-33 Teletype, sharing
the console current loop with the keyboard. Bytes from the reader and bytes from
the keys genuinely do arrive on the same serial port. It should be stated
explicitly somewhere in the docs, because a reader reviewing the code will look
for a reader device and not find one.

Two behaviours inside it:

**(a) The feed back-pressures on the FIFO.** `web/app.js:2232`:

```js
if (!this.flooding && machine.rxPending && machine.rxPending() > 400) break;
```

On the AUTO-LOAD path this means the load **can never overrun**, however badly
the loader keeps up. A real reader runs at a fixed mechanical rate and the
loader either keeps up or loses bytes. The raw `START` path does model overrun
(the `flooding` flag after ~0.75 s of a jammed FIFO); the bootstrap path does
not. Whether that matters depends on whether AUTO-LOAD is considered fully
inside the "labelled shortcut" concession.

**(b) The bootstrap is synthesised, not MITS's.** `makeBootstrap()` at
`web/app.js:2113` builds a ~40-byte loader. The 8080 in it is correct — it polls
`IN 0x10 / RRC / JNC` on RDRF (bit 0 rotated into carry), skips leader nulls
with `ORA A / JZ`, stores with `MOV M,A / INX H`, counts down `DE`, and `JMP`s to
the start address. But it is not the loader MITS published. It is clearly
labelled AUTO-LOAD, and the reader's `START` button preserves the authentic
hand-keyed path, so this reads as intentional. See §5.3 for the deramp.com
bootstrap the owner wants investigated.

**Well done:** the reader's `START` button runs the reader motor and nothing
else — no memory touched, no bootstrap, no RUN — and warns when no loader is
running because the bytes really do fall on the floor. The 128-frame blank
leader is a lovely period detail. Both are exactly right.

### 3.4 88-ACR cassette (ports 0x06/0x07) — the best-modelled device here

`retroweb/altair8800/cassette.{h,cpp}`

Status polarity is derived from the actual 8K BASIC ROM and the derivation is
cited in the file header (`IN 06 / ANI 01 / JNZ` → wait while bit 0 is set; `IN
06 / ANI 80 / JNZ` → wait while bit 7 is set; both ready conditions active-low).
The tape is genuinely linear: a physical head position, record-at-head with
overwrite, a 12-byte inter-program gap so CLOAD can resync, FF/REW winding at
30× play rate with auto-stop at both ends, and a credit-based throttle measured
against CPU cycles so 25×/50× are really that fast rather than frame-capped.
20 unit tests. 300 baud → 30 B/s is correct for KCS with 10-bit framing.

**Bug (a) — the transport freezes while the CPU is stopped.**
`web/wasm_machine.cpp:214` calls `cassette_.tick()` only from `runCycles()`, and
`web/app.js:557` only calls `runCycles()` when `running && powered`. Press STOP
on the front panel and FF/REW stop winding. On a real setup the deck is a
separate box with its own mechanical transport; the CPU's RUN/STOP paddle has no
effect on it.

**Bug (b) — panel RESET rewinds the tape.**
`web/app.js:775` binds RESET to `machine.reboot()`, which calls
`cassette_.reset()` (`web/wasm_machine.cpp:92`), which sets `pos_ = 0`
(`cassette.cpp:23`). The header comment acknowledges the design choice ("the
transport stops and the head returns to the start"), but physically this cannot
happen: RESET is a bus signal to the S-100 cards, and the cassette deck is not
on the bus. Pressing RESET on a real Altair leaves the tape exactly where it was,
still playing if PLAY is down.

The same call also does `disk_.reset()`, which homes every drive's head to
track 0 (§3.2b).

> **Owner decision (#8):** yes, make RESET realistic.

**Work item:** `Machine::reboot()` should reset the CPU and the S-100 boards'
*electrical* state only. Specifically: do not move the cassette head, do not
change the cassette transport keys, and do not move disk heads. Deselecting the
disk controller and clearing its latches on reset **is** correct — that is a bus
signal reaching the 88-DCDD card. Add tests: tape position survives RESET; disk
track survives RESET; disk controller is deselected by RESET.

### 3.5 Memory / bus model

`web/wasm_machine.cpp:256`:

```cpp
if (a < ram_top_)                 return mem_[a];
if (a >= rom_lo_ && a <= rom_hi_) return mem_[a];
return 0xFF;                      // unpopulated: floating high
```

Correct: unpopulated reads return `0xFF`, which is the Altair convention.

**Finding:** the RAM test comes *first*, and `loadBytes` only establishes a ROM
window when `addr >= ram_top_` (`web/wasm_machine.cpp:79`). So on the 64 KB CP/M
preset, `ram_top_ == 0x10000` and the 88-DCDD boot PROM at `0xFF00` is **plain
writable RAM** — the machine has no read-only region at all. On real hardware
the PROM shadows RAM and is not writable. In practice nothing writes there after
boot, so this is latent rather than active, but it means a CP/M BIOS bug that
scribbled over `0xFF00` would be silently tolerated.

Sense switches on `IN 0FFh` returning the A8–A15 paddle row is correct
(`web/wasm_machine.cpp` `bus.in`, `port == 0xFF`).

### 3.6 Front panel

The paddle set and layout match the real machine: OFF/ON, STOP/RUN, SINGLE STEP,
EXAMINE / EXAMINE NEXT, DEPOSIT / DEPOSIT NEXT, RESET / CLR, PROTECT /
UNPROTECT, and two AUX. EXAMINE loading the switches into PC and DEPOSIT writing
at PC is correct Altair behaviour. SINGLE STEP only working while stopped is
correct. The status lamp names and the WAIT/HLDA second row match the real
silkscreen.

**Finding (a) — the status lamps are decorative, not derived from 8080 machine-cycle status.**
`web/app.js:825-840`:

| Lamp | Real meaning | What it does here |
|---|---|---|
| `MEMR` | asserted during a memory-read machine cycle | "the CPU is running" |
| `M1` | asserted during an instruction-fetch cycle | "the CPU is running" |
| `INP` | asserted during an `IN` machine cycle | "the paper-tape reader is feeding" |
| `WO` | **active-low** — lit *except* during a write | hardwired `true` (comment at `:834` says otherwise) |
| `STACK` | asserted during a stack cycle | never lights |
| `OUT` | asserted during an `OUT` cycle | never lights |
| `INT` | interrupt acknowledged | never lights |
| `HLDA` | hold acknowledged | never lights |
| `PROT` | memory protect active | never lights |
| `INTE`, `HLTA`, `WAIT` | — | **correct** |

Also, the data lamps re-read `mem[lastAddr]` rather than showing the byte that
was actually on the data bus, so an `IN` displays memory contents rather than
the port value.

Fixing this properly means having the CPU core report per-machine-cycle status
bits — a real change to the `Bus` interface, not a UI patch. Worth scoping
separately.

**Finding (b) — Kill the Bit's moving bit does not render as a moving bit.**

`web/wasm_machine.cpp:232`:

```cpp
int busActivity() { int v = addr_or_; addr_or_ = 0; return v; }
```

`addr_or_` is the OR of every address touched since the last read, sampled once
per frame (`web/app.js:817`).

In Kill the Bit, `HL` is a delay counter incremented by 14 (`DAD B` with
`BC = 0x000E`) until it overflows — 65536/14 ≈ 4681 iterations at 48 T-states
each ≈ 225k cycles ≈ 6.7 frames. So within a single frame `H` sweeps roughly
`0x00`→`0x24`, and the OR of that sweep lights several of A8–A15 **at the same
brightness as `D`'s single bit**.

On real hardware the four consecutive `LDAX D` instructions put `D` on the
address bus four times per loop iteration, making its bit visibly *brighter*
than `H`'s sweep. **That brightness weighting is the entire visual effect.** An
OR cannot reproduce it.

The `baremetal` preset guide promises *"a lit bit sweeps across the top address
lamps (A8 - A15)"*, and the test in `panel-lights.spec.ts` only asserts that
`addr & 0xff00` changes between frames and that `D` holds exactly one bit — so
it passes regardless of how the lamps actually look.

> **Owner decision (#9):** independently noticed; report it.

**Work item:** replace the OR with a per-bit hit *count* per frame (a
`uint16_t counts[16]` or an accumulator array), then map count → lamp opacity so
frequently-driven address bits glow brighter. That is both closer to how the
real incandescent lamps integrated and the only way Kill the Bit reads correctly.

---

## 4. Serial terminal options — cosmetic only; owner wants full realism

`web/app.js:182` (`TERM_PROFILES`)

### What is right

The baud→characters-per-second conversion is correct and the ASR-33 detail is
genuinely good (`web/app.js:234`):

```js
baudCps = p.baud ? p.baud / (key === "tty33" ? 11 : 10) : 0;
```

The ASR-33 framed characters as 1 start + 8 data + **2** stop = 11 bits, so
110 baud is exactly 10 cps. Everything else uses 10 bits. Correct.

Also right: scrollback disabled on the CRT profiles and enabled only on the
Teletype's paper roll; the `bare` housing for Modern and Teletype; the amber
variant; the mechanical bell for the ASR-33 vs. a phosphor flash for CRTs.

### The finding

**The profiles change font, colour, bezel, bell and baud rate — and nothing
else. Every profile runs through xterm.js's full VT100/xterm parser.**

Concretely:

| Profile | Claim | Reality |
|---|---|---|
| Teletype ASR-33 | a printing terminal, no cursor addressing possible | processes the full ANSI/xterm escape set |
| DEC VT52 | VT52 command set (`ESC A/B/C/D`, `ESC Y` addressing) | full VT100+ |
| Lear Siegler ADM-3A | `ESC =` row/column addressing, no ANSI | full VT100+ |
| Glass TTY | minimal — CR/LF/BS and little else | full VT100+ |
| DEC VT100 green/amber | VT100 | roughly right by accident |

This matters for real software in the repo: the CP/M Games disk help text tells
users to *"Switch the Terminal dropdown to 'DEC VT100' first — these move the
cursor around and expect a real CRT."* Those programs would work identically
under the Teletype profile, which is physically impossible.

> **Owner decision (#3):** these should be 100% realistic. This is a
> requirement, not a nice-to-have.

**Work item — per-profile terminal emulation.** Sketch of an approach:

1. Insert a **per-profile output filter** between `outQ` and `term.write()`,
   driven by a capability descriptor on each `TERM_PROFILES` entry.
2. **ASR-33 / Glass TTY:** strip or render-as-literal every escape sequence;
   honour only CR, LF, BS, BEL, HT. On the ASR-33, BS does not erase (the print
   head backs up and overstrikes) and there is no cursor addressing, no clear
   screen, no reverse video. Uppercase-only is already handled by CAPS LOCK but
   should be forced, not optional, on this profile.
3. **VT52:** translate the VT52 set (`ESC A/B/C/D` cursor, `ESC H` home,
   `ESC J` erase to end of screen, `ESC K` erase to end of line, `ESC Y row col`
   direct addressing) into what xterm.js accepts, and drop everything else.
   xterm.js does have a VT52 mode via DECANM — worth checking whether it can be
   entered and locked rather than writing a translator.
4. **ADM-3A:** translate `ESC =` + (row+32) + (col+32) into a CUP sequence, plus
   `^Z` clear screen, `^H/^L/^K/^J` cursor moves, `^M` CR, `^N/^O` (rarely used).
   Drop ANSI. This is what WordStar's ADM-3A terminal definition emits, so it is
   directly testable against the WordStar disk already in `disks/`.
5. **VT100:** keep as-is, but verify the DECAWM/DECOM/scroll-region subset
   actually matches a VT100 rather than xterm.
6. Consider whether the correct terminal for each *preset* should be enforced
   rather than merely defaulted.

**Also:** verify the baud choices while you are in there. VT100 at 9600 and
ADM-3A at 9600 are plausible defaults; VT52 at 4800 and Glass TTY at 300 look
arbitrary. Each should be justified against a manual or changed to a documented
default.

**Test gap:** only `tty33` (10 cps) and `modern` (unmetered) have their metering
asserted, in `fidelity.spec.ts`. The other five profiles' baud rates are
completely untested — `terminal.spec.ts` only checks fonts, bezel classes and
persistence. Every profile needs a metering assertion, and once the filters
exist, an escape-handling assertion too.

---

## 5. Presets — historical accuracy

`web/app.js:1767` (`PRESETS`)

### 5.1 Per-preset findings

**`baremetal` — "Bare-Metal Toggle", Early 1975, 4 KB, ASR-33**

- Card list includes **MITS 88-2SIO**, which is a **1976** board. In early 1975
  the MITS serial card was the **88-SIO** (and 88-SIO-A/B), at ports 0x00/0x01 —
  which this emulator does not implement at all.
- *"The Altair as it first shipped"* had **256 bytes** on the 88-CPU board, not
  4 KB. The first memory board was 1 KB static.
- **"MITS 88-4K Static RAM" is not a MITS part number.** The real boards are
  88-4MCS (4K static) and 88-4MCD (4K dynamic). CLAUDE.md explicitly names
  *"hardware names on the S-100 cards"* as a fidelity feature, so this matters.
- Internal inconsistency: the blurb says *"No keyboard, no screen worth the
  name"* while the build includes a serial card and `term: "tty33"`.
- **Kill the Bit was a front-panel toggle-in program.** Delivering it on paper
  tape is an emulator conceit — there was no MITS paper tape of it, and a
  24-byte tape with no checksum loader is not a period artifact. The guide text
  does point at the manual path, but the preset's default is the tape.

**`stock` — "Stock Launch", Mid 1975, 4 KB, ASR-33**

- Same 88-2SIO anachronism; **"MITS 88-4K DRAM"** again not a real part number
  (should be 88-4MCD).
- Loads what the ROM manifest calls **"Altair 4K BASIC 4.0 (MITS 1976)"** onto a
  machine labelled "Mid 1975". Mid-1975 was 4K BASIC 1.0/2.0. Either the era or
  the software is wrong. See §5.2.

**`cassette` — "Cassette Hobbyist", 1976, 32 KB, ADM-3A — accurate**

- 88-16MCD × 2 = 32 KB is a real MITS configuration with real part numbers. ✅
- 88-ACR is real and correctly dated. ✅
- ADM-3A is 1976. ✅
- 8K BASIC on paper tape is right for 1976. ✅
- `CLOAD "S"` single-letter naming matches Altair BASIC's actual syntax. ✅
- "Realistic = 300 baud" for the cassette is right. ✅
- ~14 minutes for 8K at 10 B/s is arithmetically right and matches reality. ✅

The only quibble: the deck is labelled **"RADIO SHACK CASSETTE RECORDER"** in
the UI. That is actually *correct* framing — the 88-ACR is the S-100 interface
board and the recorder was any consumer deck — and "Realistic" being both the
speed-selector label and Radio Shack's brand name is a happy accident. No change
needed; noting it so nobody "fixes" it.

**`cpm` — "CP/M Workstation", 1978, 64 KB, VT100**

- The guide says **"CP/M 2.2 has booted"**. CP/M **2.2 is 1979**; 1978 was
  CP/M 1.4. Either change the era to 1979–80 or change the disk description.
- VT100 shipped August 1978, so it just fits 1978 — but pairs better with 1979.
- Inconsistency: the preset backplane says **"64K Static RAM (3rd party)"**
  while the disk cabinet hint (`web/app.js`, `dcdd-hint`) says CP/M *"wants all
  64 KB of RAM (4× 88-16MCD boards)"*. Pick one story.
- Note the 64 KB RAM setting also means the boot PROM at `0xFF00` is writable
  (§3.5).

### 5.2 The 88-SIO / 2SIO question — challenged, may stand

Every preset, regardless of era, ships an 88-2SIO at ports 0x10/0x11, because
that is the only serial board the emulator implements and the only one the
bundled BASIC images target.

**The challenge:** if the goal is fidelity, the 1975 presets are wrong hardware.
A genuinely period "Early 1975" or "Mid 1975" machine used the 88-SIO at ports
0x00/0x01, and MITS shipped BASIC builds targeting it. Implementing an 88-SIO is
not hard — it is a simpler board than the 2SIO — but it requires finding and
pinning SIO-targeted 4K/8K BASIC images, which may not exist in a form that can
be fetched with a checksum.

**Three ways out, in increasing cost:**

1. **Relabel.** Keep the 2SIO, but move `baremetal` and `stock` to "1976" and
   drop the 88-2SIO from the card list where it is anachronistic. Cheapest,
   loses nothing functional, and stops the docs asserting something false.
2. **Implement the 88-SIO** at 0x00/0x01 as a second board (it can share
   `Serial2SIO`'s machinery with a different port map and status layout), and
   add a preset-level choice of console board. Then source period BASIC images.
3. **Do nothing**, but document the simplification explicitly in the preset
   guide text and in CLAUDE.md, so it is a *stated* departure rather than an
   unmarked one.

> **Owner decision (#5):** challenge it, but it may stay as-is. Recorded here so
> the decision is deliberate rather than accidental.

Note that the DBL boot PROM's error path already writes to **port 0x01** (the
88-SIO data port) — see the `3E 43 / 01 / 3E 4F ... D3 01` tail of
`disk_bootrom.h`. Since nothing maps port 0x01, a boot failure currently prints
nothing at all. Implementing an 88-SIO would incidentally make CP/M boot errors
visible, which is a real diagnostic win.

### 5.3 Altair BASIC version numbers, and the deramp.com 3.2 bootstrap

**Question first:** `web/roms/manifest.json` describes the 4K image as
**"Altair 4K BASIC 4.0 (MITS 1976)"**, and the upstream file is
`nippur72/8-bit-projects/altair-basic/4kbas40.bin`. I could not confirm that a
**version 4.0 of *4K* BASIC** ever shipped. The release line I know of is 4K
BASIC 1.0 (July 1975) → 2.0 → 3.0 → 3.2 (1976), with **4.0 being an 8K/Extended
release (1977)**. The 8K image is separately described as "8K BASIC 4.0
(MITS 1976)", where 4.0 is more usually dated 1977.

If those version numbers are wrong they are wrong in four places: the two
`manifest.json` entries, `web/roms/README.md`, and the `stock` preset guide text.

**Work item — verify and, if needed, correct.** Also worth reconsidering the
`(MITS 1976)` dating on both, and reconciling it with the preset eras (§5.1).

**Owner direction (#6): investigate deramp.com's shorter bootstrap for 3.2.**

Mike Douglas's deramp.com Altair archive — already a pinned source in this repo
for the 8K EPROM build (`8kBas_e0/e8/f0/f8.bin`, see `web/roms/fetch-basic.sh`)
— also carries Altair BASIC 3.2 material and a **shorter bootstrap loader** than
the one currently synthesised in `makeBootstrap()` (`web/app.js:2113`).

The owner would like this simulated. Rationale: a shorter, *historically real*
bootstrap is strictly better than a synthesised 40-byte one for the panel-guide
hand-keying path — fewer switch flips for the user, and it is an actual artifact
rather than an invention. It would also pair naturally with a 3.2 image if the
version question above resolves toward 3.2.

**Work item:**
1. Locate the exact bootstrap on deramp.com (Mike Douglas's Altair software
   archive, alongside the ROM BASIC images already fetched by
   `web/roms/fetch-basic.sh`). **Do not guess at the byte listing — transcribe
   it from the source and cite the URL in a comment**, per CLAUDE.md's rule on
   porting behaviour.
2. Confirm which BASIC version and which console board it targets (88-SIO vs.
   88-2SIO port addresses) — this determines whether it works with the currently
   bundled 2SIO-targeted images or needs a matching 3.2 image fetched and
   checksummed alongside.
3. Replace or supplement `makeBootstrap()` with it, and update the panel guide's
   hand-keyed loader (which currently follows the synthesised loader — see
   commit `0460d11`) to teach the real one.
4. Add it to `fetch-basic.sh` with a pinned SHA-256 if it comes as a file, or
   inline it as a cited `constexpr` table if it comes as a listing.

### 5.4 Question — where does "~716 bytes free" come from?

The figure **~716 bytes free** for 4K BASIC in a 4 KB machine appears in prose
in three places: `web/roms/manifest.json` (the 4K BASIC description),
`web/roms/README.md`, and the `stock` preset guide in `web/app.js`.

> **Owner decision (#7):** not sure — report as an open question.

**Question for whoever picks this up:** was 716 observed by actually cold-starting
the bundled `4kbas.bin` in a 4 KB machine and reading what BASIC printed, or is
it an estimate? CLAUDE.md calls out *"`MEMORY SIZE?` auto-detection landing on
the real number"* as a fidelity feature, so this number is load-bearing.

**Work item either way:** add an E2E assertion that boots the `stock` preset with
Auto-load and asserts the exact "BYTES FREE" figure the ROM prints. If the
number in the docs is wrong it gets corrected once and can never drift again.

### 5.5 Test gap

`presets.spec.ts` asserts RAM size, terminal profile and visible devices for all
four presets — good. It does **not** assert the S-100 card lists, the era
strings, or the blurb/guide text. Given that the card names are where most of
the accuracy problems live (§5.1), those deserve assertions too.

---

## 6. Test adequacy

### What is strong

- **CI runs all four 8080 diagnostics** (8080PRE, TST8080, CPUTEST, 8080EXM)
  through the CP/M host, pinned by SHA-256, and exits non-zero on failure.
- **CI boots a real MITS CP/M 2.2 diskette** (`dhansel/Altair8800` DISK01, pinned
  by SHA-256) on the emulated 88-DCDD and runs `DIR` and `STAT`.
- **~213 Playwright tests** across 17 spec files drive the real UI in a browser.
- `fidelity.spec.ts` encodes CLAUDE.md's realism rules as executable checks —
  2 MHz pacing, ASR-33 metering, paper tape at Realistic vs Max, turbo not
  surviving a keypress. This is exactly the right instinct.
- A red web-test run blocks the deploy (`build` job `needs:` it).
- Coverage targets exist for both C++ (llvm-cov, including the diagnostic COMs)
  and JS (V8/monocart).

### Concrete gaps

| # | Gap | Impact |
|---|---|---|
| 1 | **No unit test for `serial2sio.cpp`.** The GoogleTest suite is `arithmetic_test` (27), `cassette_test` (20), `disk88_test` (8). The 2SIO's status bits, master reset, overrun latch, channel B and IRQ decoding have no direct test. | The console board is the single most load-bearing peripheral; it is only tested implicitly. |
| 2 | **No T-state assertions** (§2.5). | A wrong cycle count changes every real-time behaviour and no test goes red. |
| 3 | **`Cpu::interrupt()` untested and uncalled** (§2.3). | Whole feature is unverified. |
| 4 | **Disk edges untested:** the 138th byte, `reset()` homing heads, empty-drive status (§3.2). | All three confirmed to exist; none caught. |
| 5 | **Five of seven terminal baud rates unmetered** (§4). | Only `tty33` and `modern` are asserted. |
| 6 | **Preset card lists / era metadata unasserted** (§5.5). | Where most accuracy bugs live. |
| 7 | **`make check` = `demo + test` only.** The diagnostics run in CI (`make cpm`) but not from the local gate. | A local `make check` can pass while the core is broken. |
| 8 | **`if (location.search.includes("test"))`** (`web/app.js:2679`) matches any query string containing the substring "test", not just `?test=1`. | Fragile; a future param like `?latest=` would silently enable the test seam and force Max load speeds in production. |
| 9 | **Kill the Bit lamp test asserts the wrong thing** (§3.6b): it checks that `addr & 0xff00` changes and that `D` has one bit — both true regardless of whether the lamps render correctly. | The marquee visual is unverified. |

---

## 7. Documentation discrepancies

### 7.1 CLAUDE.md contradicts the code on the SIMH track-0 latch

> **Owner decision (#1):** report this discrepancy.

`CLAUDE.md:17` instructs:

> *"keep the quirk, including documented bugs that software depends on (e.g. the
> SIMH 88-DCDD track-0 latch)."*

`retroweb/altair8800/disk88.cpp:143` does the opposite, deliberately:

```cpp
// The track-0 line is a physical sensor: keep it honest rather than
// latch it like SIMH does (a stale flag breaks BIOSes whose head-home
// routine trusts it, e.g. Burcon CP/M's seek0).
```

and `retroweb/altair8800/README.md` documents the same deviation:

> *"The controller's track-0 status line is corrected to follow the head instead
> of latching (SIMH's behaviour), which is what real 8-inch CP/M BIOSes need to
> find head-home."*

**The code is right and CLAUDE.md's example is wrong.** SIMH's `altair_dsk.c`
only ever *sets* the track-0 flag (on a step-out that hits zero) and never
clears it on a step-in — that is a SIMH artifact, not a MITS hardware behaviour.
Real 88-DCDD hardware reads track 0 from a physical opto sensor, so the line
follows the head in both directions. Following the real hardware is the correct
call under CLAUDE.md's own "realism is the default" rule; it is only the
*example* that is stale.

**Work item:** correct `CLAUDE.md:17` — replace the track-0 latch with a genuine
"keep the bug" example, or reword to make clear that SIMH artifacts are not
themselves period behaviour. Leave `disk88.cpp` and the README alone.

### 7.2 Smaller doc inconsistencies

- CP/M RAM story told two ways: "64K Static RAM (3rd party)" in the preset
  backplane vs. "4× 88-16MCD boards" in the disk cabinet hint (§5.1).
- CLAUDE.md's `?test=1` clause mentions forcing **floppy** load speed to Max;
  there is no floppy speed control (§3.2d).
- `printManual()` (`web/app.js:467`) temporarily raises `baudCps` above the
  profile's real rate so the how-to page prints in ~8 s instead of ~2 min on a
  Teletype. It only affects emulator UI text, never machine output, and a
  keypress skips it — but under CLAUDE.md's three-rule test it is an unlabelled
  speed override. Worth one line of UI text ("press a key to skip") or an
  explicit carve-out in CLAUDE.md.

---

## 8. Architecture assessment

**Good, and it earns the praise.**

- The CPU core is genuinely host-agnostic: it talks to the world only through
  four `std::function` callbacks. The same core drives the GoogleTest fixture,
  the CP/M diagnostic host, the native disk-boot harness, and the browser.
- Peripherals are independent units with a uniform `owns() / in() / out()`
  shape, composed by `Machine::make_bus()` in a readable dispatch chain.
- The C++ core builds and tests with no emscripten dependency, which is why
  `make check` can be a fast local gate.
- Media (`*.bin`, `*.dsk`) is fetched at build time with pinned SHA-256s rather
  than committed. Correct call, and the CI cache keying on the workflow file
  hash is neat.
- Provenance is cited in headers wherever behaviour was ported.

**Two things worth improving:**

1. **`Machine::reset()` vs. `Machine::reboot()`** are confusingly named for two
   quite different operations. `reset()` wipes all 64 KB of RAM and re-seeds the
   echo ROM; `reboot()` resets the boards and CPU without touching memory. The
   front panel's RESET paddle correctly calls `reboot()`, but the naming invites
   the wrong one. Suggest `powerOnReset()` / `busReset()` or similar.
2. **`web/app.js` is 2,721 lines** in a single `boot()` closure holding the front
   panel, three device UIs, the preset system, the loader state machine, the
   terminal metering and the test seam. The device objects (`paperTape`, `tape`,
   `cassette`, `disk`) are already well-factored *inside* it and would split into
   modules cleanly. Not urgent, but it will get worse.

---

## 9. Prioritised work list

Ordered by owner priority and impact. Each item names the decision behind it.

### P0 — owner-directed, changes behaviour

1. **Make the terminal profiles real** (§4). *Owner: "100% realistic."* Add a
   per-profile output filter: ASR-33 and Glass TTY strip escapes entirely and
   overstrike on BS; VT52 handles only the VT52 set; ADM-3A handles `ESC =`
   addressing and control-char cursor moves, no ANSI. Test each profile's escape
   handling **and** its baud metering. Validate ADM-3A against the WordStar disk
   already in `disks/`.
2. **Make RESET realistic** (§3.4, §3.2b). *Owner: "yes."* `Machine::reboot()`
   must not move the cassette head, must not change transport keys, and must not
   home disk heads. Deselecting the disk controller **is** correct. Add tests for
   all three.
3. **Wire up interrupts + the `EI` delay** (§2.2, §2.3). *Owner: "I think so."*
   `sio_.on_irq` → `cpu_.interrupt(0xFF)` (RST 7). Add a `pending_ei` flag so
   `EI` takes effect one instruction later. Test: DI-ignored, EI-taken,
   HALT-wake, INTE-cleared-on-accept, EI-delay.
4. **Fix the Kill the Bit lamps** (§3.6b). *Owner: independently noticed.*
   Replace `addr_or_` with per-bit hit counts per frame and map count → lamp
   brightness. Update the test to assert the moving bit is brighter than the
   `HL` sweep.
5. **Cassette transport should run while the CPU is stopped** (§3.4a). Move
   `cassette_.tick()` out of `runCycles()` so FF/REW wind with the panel on STOP.

### P1 — owner-directed, investigation

6. **Verify the Altair BASIC version numbers** (§5.3). Is there really a 4K
   BASIC *4.0*? Fix `manifest.json` ×2, `roms/README.md`, and the `stock` guide
   if not.
7. **Source and simulate deramp.com's shorter 3.2 bootstrap** (§5.3). *Owner
   direction.* Transcribe from the source, cite the URL, confirm the target
   console board, pin it, and teach it in the panel guide in place of the
   synthesised loader.
8. **Answer the "~716 bytes free" question** (§5.4). Then lock it in with an E2E
   assertion on the exact figure the ROM prints.
9. **Decide the 88-SIO question** (§5.2). Relabel the 1975 presets, implement the
   88-SIO, or document the simplification. *Owner may keep as-is — but make it a
   recorded decision.* Note that implementing it would also make DBL boot errors
   visible (they currently go to unmapped port 0x01).

### P2 — reported findings, no decision yet

10. **88-DCDD has no timing model** (§3.2d). *Owner: "report as a potential
    issue."* If addressed: a `LOAD SPEED` selector on the disk cabinet matching
    the paper-tape/cassette pattern, gated on CPU-cycle credit.
11. **Correct CLAUDE.md's track-0 example** (§7.1). *Owner: "please report that
    discrepancy."* The code is right; the doc's example is stale.
12. **Disk edge cases** (§3.2a, §3.2c): the 138th byte per sector; an empty
    drive reporting a healthy status. Both confirmed empirically. Add tests
    either way, so the current behaviour is at least pinned.
13. **Front-panel status lamps** (§3.6a): `MEMR`/`M1`/`INP`/`WO`/`STACK`/`OUT`
    are decorative. Proper fix needs per-machine-cycle status from the CPU core —
    scope separately.
14. **Boot PROM is writable at 64 KB** (§3.5): reorder the RAM/ROM checks in
    `bus.read`, or let the ROM window win regardless of `ram_top_`.
15. **Preset metadata fixes** (§5.1): real MITS part numbers (88-4MCS / 88-4MCD),
    CP/M 2.2 = 1979, the 64 KB RAM story told one way, the `baremetal` blurb
    contradiction.

### P3 — test hardening

16. Unit tests for `serial2sio.cpp` (§6.1).
17. T-state assertions for all 256 opcodes plus taken/not-taken conditionals (§2.5).
18. Baud metering assertions for all seven terminal profiles (§6.5).
19. Preset card-list and era assertions (§5.5).
20. Add `make cpm` to `make check` (§6.7).
21. Tighten the test-seam guard to a real query-param parse (§6.8).

---

## Appendix A — file map

| Path | Role |
|---|---|
| `retroweb/altair8800/i8080.{h,cpp}` | 8080 core — registers, flags, cycle table, decoder, `Bus` callbacks |
| `retroweb/altair8800/serial2sio.{h,cpp}` | 88-2SIO, ports 0x10–0x13, ring-buffered |
| `retroweb/altair8800/ringbuffer.h` | fixed-capacity SPSC byte ring |
| `retroweb/altair8800/cassette.{h,cpp}` | 88-ACR, ports 0x06/0x07 |
| `retroweb/altair8800/disk88.{h,cpp}` | 88-DCDD, ports 0x08–0x0A, SIMH-derived |
| `retroweb/altair8800/disk_bootrom.h` | 256-byte MITS DBL boot PROM at 0xFF00 |
| `retroweb/altair8800/main.cpp` | native echo harness |
| `retroweb/altair8800/cpm/cpm_host.cpp` | CP/M BDOS shim for the diagnostic `.COM`s |
| `retroweb/altair8800/cpm/cpm_disk.cpp` | native 88-DCDD CP/M boot test |
| `retroweb/altair8800/tests/` | GoogleTest: `arithmetic_test`, `cassette_test`, `disk88_test`, `interrupt_test` |
| `retroweb/altair8800/web/wasm_machine.cpp` | embind wrapper composing CPU + boards |
| `retroweb/altair8800/web/app.js` | front end: panel, devices, presets, loaders, terminal |
| `retroweb/altair8800/web/index.html` | markup + CSS for the panel, disk cabinet, deck |
| `retroweb/altair8800/web/tests/` | 17 Playwright specs, ~213 tests |

## Appendix B — key line references

| Finding | Location |
|---|---|
| Cycle table | `i8080.cpp:11-28` |
| `Cpu::reset()` clears all registers | `i8080.cpp:27` |
| `sub` AC (inverted borrow) | `i8080.cpp:59` |
| `ana` AC quirk | `i8080.cpp:74` |
| `daa` | `i8080.cpp:112` |
| `Cpu::interrupt()` — no caller | `i8080.cpp:213` |
| `EI` — no delay | `i8080.cpp:411` |
| 512-byte FIFOs | `serial2sio.h:44` |
| `on_irq` fired but never assigned | `serial2sio.cpp:47`, `:91` |
| `Disk88::reset()` homes heads | `disk88.cpp:26` |
| 138th byte per sector | `disk88.cpp:91` |
| Empty drive reports healthy | `disk88.cpp:124` |
| Track-0 sensor (correct, contra CLAUDE.md) | `disk88.cpp:143` |
| `CassetteACR::reset()` rewinds | `cassette.cpp:23` |
| ROM window only above `ram_top_` | `web/wasm_machine.cpp:79` |
| `Machine::reboot()` | `web/wasm_machine.cpp:92` |
| `cassette_.tick()` inside `runCycles()` | `web/wasm_machine.cpp:214` |
| `busActivity()` — OR, not brightness | `web/wasm_machine.cpp:232` |
| RAM checked before ROM window | `web/wasm_machine.cpp:256` |
| `TERM_PROFILES` | `web/app.js:182` |
| ASR-33 11-bit framing | `web/app.js:234` |
| `printManual` baud override | `web/app.js:467` |
| turbo boost ×12 | `web/app.js:560` |
| `busActivity()` sampled per frame | `web/app.js:817` |
| Status lamps | `web/app.js:825-840` |
| RESET paddle → `reboot()` | `web/app.js:775` |
| `PRESETS` | `web/app.js:1767` |
| Paper-tape FIFO back-pressure | `web/app.js:2232` |
| `makeBootstrap()` | `web/app.js:2113` |
| Test seam guard | `web/app.js:2679` |
| CLAUDE.md track-0 latch claim | `CLAUDE.md:17` |
