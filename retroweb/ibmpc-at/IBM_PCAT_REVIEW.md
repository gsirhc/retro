# IBM PC/AT (5170-339) — design notes and open questions

This document plays the same role as `retroweb/altair8800/ALTAIR_REVIEW.md`
and `retroweb/cg-oac-6502/CGOAC6502_REVIEW.md`: a citation trail for the
decisions behind the emulator's behavior, and an honest list of what isn't
built yet, so a future contributor sees the reasoning instead of an
unexplained gap.

## 0. Status: Phase 2 well underway (chipset + real BIOS booting partway)

Phase 1 (`cpu80286.h`/`.cpp` + tests) is done. Phase 2's chipset devices are
built and tested: `pic8259` (×2, cascaded), `pit8253`, `i8042` (keyboard
controller, A20 gate, CPU-reset trick, the keyboard's own unsolicited
post-reset BAT byte), `cmos_rtc` (MC146818), `dma8237` (×2, register file
only — no live transfer yet), `chipset` (the glue/memory-decode layer), and
`machine` (wires the CPU + chipset and drives both from `run_cycles()`).
87/87 GoogleTest cases pass, including an end-to-end PIT→PIC→CPU interrupt
round trip that wakes a HLTed CPU and a regression test for the
CPU-reset-must-not-corrupt-pacing bug class `cg-oac-6502`'s review doc §10
documents.

The real, freely-licensed system BIOS (`BIOS-bochs-legacy`, see §6) is
fetched, checksummed, and boots against this chipset via a native
diagnostic harness (`bios_host.cpp`, built ahead of schedule) far enough to
write a genuine POST diagnostic code before reaching a stable wait loop —
see §6 for the exact state and what's still unexplained there.

**Not yet done**: nothing boots to a usable state yet (no floppy/HDD/EGA),
there's no WASM/browser build, and the `F000:0C49` wait loop in §6 isn't
diagnosed. See the approved plan
(`/Users/chrisgall/.claude/plans/golden-wishing-walrus.md`) for the
remaining roadmap: floppy+EGA-text boot-to-prompt (3), WD1003 HDD with a
pre-loaded FreeDOS image (4), full EGA graphics (5), PC speaker (6), full
front end (7), test suites + site integration (8).

## 1. Scope: real-address-mode only

PC-DOS 3.30 and FreeDOS never leave real mode on this hardware, so the core
implements real mode exclusively. Deliberately unimplemented (protected-mode
only, no real-mode-legal behavior worth emulating): `LGDT/SGDT`, `LIDT/SIDT`,
`LLDT/SLDT`, `LTR/STR`, `LMSW/SMSW`, `ARPL`, `LAR`, `LSL`, `VERR/VERW`,
`CLTS`, and all descriptor/gate/TSS machinery. The two-byte `0x0F` escape
(where all of these live) is decoded as a 3-cycle no-op rather than
implementing any of them.

**Deferred, not forgotten**: the well-documented 80286 erratum where a
segment's cached descriptor limit is not reset to `0xFFFF` on a return to
real mode by any path other than RESET. This is only observable if the core
ever *enters* protected mode even transiently — which it structurally
cannot, since there is no GDT/LDT/descriptor-cache machinery to hold a stale
limit in the first place. Revisit only if a later phase ever adds protected-
mode support (not currently planned).

## 2. Real-mode-legal 80286-over-8086 additions implemented

`PUSHA`/`POPA` (0x60/0x61), `BOUND` (0x62), `PUSH imm8/imm16` (0x6A/0x68),
`INS`/`OUTS` (0x6C-0x6F), three-operand `IMUL reg,r/m,imm` (0x69/0x6B),
shift/rotate-by-immediate (0xC0/0xC1, alongside the 8086's by-1/by-CL forms
at 0xD0-0xD3), `ENTER`/`LEAVE` (0xC8/0xC9). Source: Intel iAPX 286
Programmer's Reference Manual (1987), "Instruction Set Differences from the
8086."

**Quirks preserved on purpose** (real, documented CPU-generation behavior,
not bugs):
- `PUSH SP` pushes the *post-decrement* value on the 286, unlike the 8086
  (pre-decrement) — `push_reg()` in `cpu80286.cpp`, tested explicitly in
  `PushSpPushesDecrementedValue`. Software of the era used exactly this
  instruction to CPU-detect an 8086 vs. a 286-or-later at runtime.
- Shift/rotate counts are masked mod 32 on the 286; the 8086 used the raw
  unmasked count (so a shift by 200 took 200 cycles on real 8086 silicon).
  Tested in `ShiftCountMaskedMod32`.
- `INC`/`DEC` never touch `CF` — true on the whole 8086 lineage, easy to get
  wrong when reusing an `ADD`/`SUB` helper for them. Tested explicitly.
- `OF` after a multi-bit shift/rotate (count != 1) is left **undefined** by
  Intel's own documentation for this family; the core simply leaves the bit
  untouched rather than guessing, matching the documented contract.
- `AF` after `AND`/`OR`/`XOR` is likewise architecturally undefined and left
  untouched, for the same reason.

## 3. Known simplifications (documented, not silent)

- **RESET vector**: real silicon aliases the top of its 16MB address space
  down to physical `0xFFFFF0` on RESET, then relies on CS's hidden
  descriptor-cache base (not the visible CS *value*) to make that work
  before any far jump reloads CS normally. This core has no descriptor
  cache (real-mode-only, §1), so it approximates the observable effect the
  way every real-mode-only 8086-family core does: `CS=0xF000, IP=0xFFF0`
  giving physical `0xFFFF0` directly — one hex digit short of the genuine
  286's `0xFFFFF0`, but equivalent for any real AT BIOS, which always
  far-jumps to a normal `F000:xxxx` entry point within its first few
  instructions anyway.
- **`POPF`/`IRET` flag mask** (`0x0FD5`) restores CF/PF/AF/ZF/SF/TF/IF/DF/OF
  from the popped word but always zeroes IOPL and NT rather than loading
  them from the stack. Those bits have no functional effect without
  protected-mode privilege checking (which this core doesn't have, §1), so
  this is a no-op simplification today — revisit only alongside any future
  protected-mode work.
- **Cycle counts are categorical, not the full effective-address-dependent
  table.** Real 80286 timing charges a different EA-calculation cost per
  addressing mode (e.g. base+index+displacement costs more than a bare
  register indirect) on top of the 1-wait-state memory penalty. This core
  uses two flat constants (`CYC_REG=2`, `CYC_MEM=7`) folding in a
  representative EA cost + the wait state, plus per-instruction-class
  constants for jumps/calls/INT/string ops, all drawn from the iAPX 286
  timing appendix's typical values. This gets the right order of magnitude
  and throughput characteristics (no instruction is free, memory access
  costs more than register access, taken branches cost more than
  not-taken) without claiming cycle-exact precision. Revisit if a later
  phase's BIOS timing loop (e.g. the PIT-driven delay POST uses) turns out
  to be sensitive to the difference.

## 4. Opcode coverage gaps

Not implemented (falls through to the "unimplemented opcode" 2-cycle no-op
default case, or the explicit `0x0F`/`0xD8-0xDF` handling noted above):
- The protected-mode `0x0F` two-byte space (§1), **except `0x0F 0x80-0x8F`**
  (`Jcc rel16`) — an 80386 addition, not genuine 80286 behavior, but
  decoded anyway as a pragmatic compatibility concession for the prebuilt
  BIOS substitute this machine boots; see §6's investigation for why.
- `0xD8`-`0xDF` (x87 coprocessor ESC) decode the ModR/M byte (so instruction
  length stays correct for whatever follows) but perform no FPU operation —
  correct behavior for this system's spec, which has no 80287 installed.
- `0xF1` (undocumented ICEBP/INT1) is a no-op rather than raising a
  single-step-like trap.
- `AAM`/`AAD` are implemented for the standard base-10 encoding but not
  exhaustively tested against non-standard bases.

None of these are expected to matter for PC-DOS/FreeDOS or ordinary
DOS-era compiled code; flagged here so a gap is a documented decision, not
a silent one, per this repo's fidelity conventions (`CLAUDE.md`).

## 5. Chipset devices (Phase 2): scope and simplifications

- **PIC (8259 ×2)**: fixed-priority mode only (IR0 highest), normal
  (non-rotating, non-special-mask) EOI. Cascading is wired in `chipset.cpp`
  the way real hardware does: the master's IR2 input tracks
  `slave.has_interrupt()`, and an INTA cycle whose highest-pending master
  line is IR2 forwards the actual vector fetch to the slave. Real DOS-era
  BIOS/software never programs rotating priority or special mask mode, so
  they're not implemented.
- **PIT (8253)**: models a uniform symmetric toggle for every mode (a full
  period every `reload` PIT clocks) rather than each mode's exact waveform
  — correct for the AT's actual dependency (IRQ0's ~18.2 Hz edge rate,
  confirmed exactly via divisor-0/65536 in `Pit8253Test`), not for a
  cycle-perfect oscilloscope trace of e.g. Mode 2's asymmetric pulse.
- **i8042**: A20 gate defaults **disabled** at reset (a genuine AT starts
  out wrapping at 1MB like an 8086; BIOS enables A20 early in POST) — get
  this backwards and every "does A20 wraparound work" assumption in
  real-mode software breaks. The CPU-reset trick (Output Port bit 0, or
  command 0xFE) is exposed as `reset_requested()`/`clear_reset_request()`
  for the embedding `Machine` to act on, matching this codebase's
  host-agnostic device convention (a device never reaches into the CPU
  object directly).
- **CMOS/RTC**: no live ticking clock (nothing in these phases reads the
  time-of-day repeatedly); register A's update-in-progress bit is hard tied
  to 0 so a BIOS's "wait for UIP to clear" POST loop can never hang.
- **DMA (8237 ×2)**: register file (address/count/mode/mask/command) only —
  no live memory↔device transfer yet. That lands in Phase 3 alongside
  `fdc765.h`, the first device that actually needs to move bytes over
  channel 2.
- **`Machine::run_cycles()`** only polls/acknowledges the PIC when the
  CPU's `IF` flag is set, mirroring real hardware (the CPU never begins an
  INTA cycle otherwise) — polling unconditionally would wrongly consume a
  pending IRQ (e.g. a timer tick) during a `cli`-protected section that
  never actually got serviced.

## 6. BIOS integration: resolved on prebuilt pinned binaries

Phase 2's plan called for building **SeaBIOS** from a pinned source commit.
Verified this session: network access works in this sandbox, but building
SeaBIOS doesn't — it needs a real i386-targeting compiler (16-bit real-mode
codegen) plus either the `dev86` toolchain (`as86`/`ld86`/`bcc`) or a
sufficiently i386-aware GCC, and this sandbox only has Apple Clang
targeting `arm64-apple-darwin` (none of `as86`, `ld86`, `bcc`, `iasl` are
present). SeaBIOS itself also publishes no prebuilt binaries.

**Decided (user's call): always prefer a pinned, checksummed prebuilt
binary over a source build when the choice arises.** Switched to the
**Bochs Project's** own firmware instead of SeaBIOS, for two reasons: (a)
it's committed as a **prebuilt binary directly in the Bochs source tree**
(`bochs/bios/BIOS-bochs-legacy`, `bochs/bios/VGABIOS-lgpl/VGABIOS-lgpl-
latest.bin`), so there's no toolchain problem to solve at all; (b)
`BIOS-bochs-legacy` is specifically the plain-ISA/no-PCI/no-ACPI variant —
a better match for a genuine AT-class machine than SeaBIOS's PCI-oriented
default build target would have been anyway. Both are LGPL (`Copyright (C)
2001-2026 The Bochs Project`), pinned to commit
`bochs-emu/Bochs@ff17a0c2bbabccf96d33af4e08ba8061889b079d`, fetched and
SHA-256-verified via `roms/fetch-bios.sh` (same pattern as
`fetch-basic.sh`), gitignored like every other fetched binary in this
repo. `BIOS-bochs-legacy` is exactly 64KB (fills F0000-FFFFF exactly);
`VGABIOS-lgpl-latest.bin` is exactly 32KB (fills C0000-C7FFF exactly) —
loading it into the C0000 ROM window is deferred to Phase 3/5 until the
EGA device exists to back it (loading it now would just make POST hang
inside video-BIOS init code polling hardware that isn't there yet).

**Labelling** (per CLAUDE.md's substitution rule): `VGABIOS-lgpl-latest.bin`
is a full VGA-compatible video BIOS, not an EGA-only one (no maintained
EGA-only open BIOS project exists, and this is a prebuilt binary we can't
trim down ourselves) — it will expose more video modes than genuine EGA
hardware has. The EGA device itself (Phase 5) is what actually enforces
the real 640×350×16 EGA ceiling regardless of what modes this BIOS thinks
it can offer; this needs a clear boot-banner/README label once the web
front end exists.

**Verified with a real native harness** (`bios_host.cpp`, the `dos_host`-
style diagnostic host the plan called for, built ahead of schedule since it
was needed to answer this question at all): boots the actual fetched
`BIOS-bochs-legacy` image against `chipset`/`machine` and reports POST
progress via port 0x80 codes and HLT state. Result: real BIOS machine code
executes correctly against `pic8259`/`pit8253`/`i8042`/`cmos_rtc`/`dma8237`
for ~1.9M cycles, writes POST code `0x31`, and then reaches a stable
`HLT`-based wait loop at `F000:0C49`. Along the way this caught and fixed a
real gap: a genuine AT keyboard sends `0xAA` **unsolicited** after its own
power-on self-test (independent of the controller's own `0xAA` self-test
command) — BIOS's keyboard-presence check waits for exactly that byte, and
without it POST hung at an earlier point entirely (`i8042.cpp`'s `reset()`,
regression-tested in `ResetDeliversUnsolicitedKeyboardBatByte`).

**`F000:0C49` diagnosed, and fixed.** Traced the exact instructions leading
into it (single-instruction stepping with a ring-buffer trace, no
disassembler needed): it's a deliberate `CLI ; HLT` fatal-halt path,
reached from an expansion-ROM-scan loop that walks `0xC000-0xDFFF` looking
for a `0x55 0xAA` option-ROM signature (correct behavior — we haven't
loaded any option ROM yet, so it should legitimately find nothing and
continue). The loop's condition test used `0x0F 0x85` (`JNZ rel16`) — **an
80386 addition, not genuine 80286 behavior** (Intel never defined `0x0F
0x80-0x8F` on the 286; only the short `Jcc rel8` at `0x70-0x7F` exists
there). `BIOS-bochs-legacy`, despite its "legacy"/no-PCI branding, was
evidently built assuming at least a 386 baseline — unsurprising in
hindsight, since essentially nothing in the modern PC-emulation/
virtualization ecosystem still targets genuine 80286 compatibility.
`cpu80286.cpp` now decodes `0x0F 0x80-0x8F` as `Jcc rel16`, explicitly
commented and tested (`JccNear0FEncodingTaken/NotTakenWhenZero`) as a
**pragmatic BIOS-compatibility concession, not real 80286 behavior** —
the same labelled-departure discipline CLAUDE.md requires of any other
realism override.

With that fix, POST no longer halts there — it proceeds until it executes
`XOR BX,BX ; INT 10h` (video services) at `F000:0679`. That crashes into
segment `0000:0000` and runs off into the weeds, because **there is no
video BIOS loaded yet** — INT 10h's vector (physical `0x40`) is still zero
from reset, exactly the outcome the "don't load `vgabios` before the EGA
device exists" decision above anticipated, just manifesting as a wild
jump into unmapped vector space rather than a hang. This is not a new bug
to chase: it's Phase 3/5's actual job (EGA text mode + `vgabios` loaded at
`C0000`), and confirms the chipset is solid enough to get real BIOS code
this far. `trace_host.cpp` (the scratch tool used for this investigation)
has been removed now that the mystery is resolved, per its own header
comment's promise; `bios_host.cpp` (the lasting diagnostic harness) stays.

- **WD1003 `IDENTIFY DEVICE` vs. CMOS fixed-disk-parameter-table**: whether
  the chosen BIOS's ATA driver needs an `IDENTIFY DEVICE` (`0xEC`) response
  or can be steered onto an old-style CMOS type-table path is unresolved
  until Phase 4, once the actual BIOS source is read.

## 7. Phase 3: FDC + EGA text mode + real-boot debugging (in progress)

Built and fully tested this round: `fdc765` (NEC uPD765 floppy controller,
both drives, a real command/parameter/execution/result state machine),
live DMA channel-2 transfer support added to `dma8237` (address/count
advance with the real page-boundary-wrap and N-1 terminal-count
conventions), and `ega` (CRTC/Sequencer/Graphics Controller/Attribute
Controller register files, the color text-mode VRAM window, the Input
Status 1 retrace toggle, the AC address/data flip-flop and its
reset-on-reading-0x3DA quirk). All wired into `chipset`, which orchestrates
the FDC↔DMA↔memory handoff the same way it orchestrates the PIC cascade.
104/104 GoogleTest cases pass, including a chipset-level test proving a
full floppy READ DATA command actually moves real bytes from a mounted
disk image into system RAM via DMA.

**Real-BIOS boot debugging, using the fetched `BIOS-bochs-legacy` source
(same pinned commit) to pin down each blocker precisely rather than
guessing from raw bytes:**

1. Confirmed and fixed (§6): `0x0F 0x80-0x8F` (`Jcc rel16`, an 80386
   addition) needed for the option-ROM scan loop.
2. **Keyboard POST sequencing, fixed.** Fetched `rombios.c` and found the
   real `keyboard_init()` routine (line ~1965 at the pinned commit): it
   sends the keyboard an explicit `0xFF` RESET command and expects **two
   separate responses** — an immediate ACK (`0xFA`), then, on a second,
   later read, the Basic-Assurance-Test-passed byte (`0xAA`) — not the
   power-on unsolicited BAT byte the earlier fix (§0) modeled. `i8042.cpp`
   now special-cases command `0xFF` to queue `0xAA` right after its `0xFA`
   ACK is read (`ResetCommandGetsAckThenBatByteOnSeparateReads`). With
   this, `keyboard_init()` runs to completion (POST code reaches `0x77`,
   its own final checkpoint, confirmed against the source) instead of
   hanging.
3. **Real progress milestone**: with both fixes in place and
   `VGABIOS-lgpl-latest.bin` loaded at `0xC0000`, the system BIOS's
   `rom_scan` genuinely finds and far-calls into the video BIOS's real
   init routine (`C000:0003`), which correctly installs its own INT 10h
   vector — confirmed by tracing a live call/return loop between the
   system BIOS's `wrch()` helper and the video BIOS's own INT 10h handler
   that's exactly `_print_bios_banner` printing the real "Bochs BIOS..."
   banner character-by-character through **genuine, unmodified vgabios
   code** — not a stub, the actual fetched binary running correctly
   against our EGA register model.
4. **The "IVT corruption" was a symptom, not a bug — root cause found and
   fixed: a whole missing dimension of opcode coverage, not one opcode.**
   Rather than keep tracing individual crash sites by hand, added a
   permanent diagnostic hook (`Cpu::on_unimplemented`, called with the
   exact opcode and address whenever `step()` falls through to the
   unimplemented-opcode path) and ran the real BIOS boot through it. One
   opcode lit up: **`0x66`**, the 80386 operand-size override prefix —
   hit **5,818 times** in the first million steps alone. `BIOS-bochs-
   legacy` uses genuine 32-bit registers (EAX/EBX/...) throughout, even in
   16-bit real-mode code, via this prefix — not a rare edge case, a
   pervasive assumption. This is *why* the earlier "IVT corruption" showed
   up: every `0x66`-prefixed instruction was being treated as a bare 1-byte
   no-op, desyncing the entire instruction stream from that point on and
   scribbling wherever the misinterpreted bytes happened to land, including
   over the IVT.

   Fixed properly rather than patched around: `cpu80286`'s general-purpose
   registers are now stored as full 32 bits (`ax`/`bx`/... are simply the
   low 16 of `EAX`/`EBX`/...), the `0x66` prefix is recognized, and every
   opcode that fetches an immediate or writes a register/memory operand
   (the ALU group, `MOV`, `PUSH`/`POP`, `INC`/`DEC`, shifts, `IMUL`,
   string ops, `CBW`/`CWD`→`CWDE`/`CDQ`, `MUL`/`IMUL`/`DIV`/`IDIV`) now
   branches on operand size. Also added the 386's most commonly-compiled
   `0x0F`-prefixed instructions the same diagnostic hook would otherwise
   have flagged next: `SETcc`, two-operand `IMUL`, `MOVZX`, `MOVSX`. All
   of this is explicitly labelled in `cpu80286.h`'s file header as a
   80386 compatibility *concession* — not genuine 80286 behavior, kept
   because the firmware substitute needs it, exactly like the `0x0F
   0x80-0x8F` fix in finding #1. 25 new GoogleTest cases cover it
   (`OpSize32*`, `Movzx*`, `Movsx*`, `Setcc*`, `TwoOperandImul*`).
   **Deliberately not added**: a 32-bit *address* size (`0x67`, SIB bytes,
   "big real mode") — the diagnostic hook never flagged it as missing, so
   there's no evidence this machine's BIOS needs it.

5. **With that fix, the real BIOS now boots to its own correct, legitimate
   conclusion — proven by reading its actual output.** Logging every
   character the BIOS sends through its `wrch()` character-print helper
   reconstructs its real startup banner, produced by genuine, unmodified
   `BIOS-bochs-legacy` and `vgabios` code running against this machine's
   chipset:
   ```
   Bochs 3.0.devel BIOS - build: 08/23/26
   Options: apmbios pcibios pnpbios eltorito
   Press F12 for boot menu.
   FATAL: No bootable device.
   ```
   That "FATAL" is not a bug — it's the **correct** response to a machine
   with no floppy or hard disk image mounted, which is exactly this
   machine's current state (Phase 4's pre-loaded FreeDOS HDD image hasn't
   landed yet). The CPU, chipset, keyboard, PIT/PIC, DMA, FDC, and EGA are
   all now demonstrated working together well enough for a real, unmodified
   third-party BIOS to run its complete POST sequence correctly and reach
   its true terminal state.

This closes out the BIOS-compatibility debugging thread for Phase 3: three
real, cited bugs found and fixed (the `0x0F 0x80-0x8F` opcode, the keyboard
ACK+BAT sequencing, and the `0x66` operand-size dimension), each confirmed
against the actual `rombios.c` source or the BIOS's own reconstructed
output rather than guessed.

## 8. Real FreeDOS floppy fetched and mounted — booting confirmed starting

Fetched the FreeDOS 1.3 official "Floppy Edition" 1.2MB (`120m/`) boot
image via `disks/fetch-freedos.sh` (same pinned-URL + SHA-256 pattern as
`roms/fetch-bios.sh`) — its size (1,228,800 bytes = 80×2×15×512) matches
Drive A:'s geometry exactly, no conversion needed. Mounted it via
`chipset.fdc.mount(0, ...)` and re-ran the boot: still got **"FATAL: No
bootable device"** even with a real, bootable floppy physically mounted.

Root cause, found in `rombios.c`'s `int19_function` (not guessed): with
`BX_ELTORITO_BOOT` compiled in (confirmed earlier — "eltorito" is in this
BIOS's own printed options line), the boot device sequence comes from
**CMOS register 0x3D** (low nibble = 1st boot device; `0x01` = floppy,
`0x02` = hard disk), not from probing drives directly. We'd never seeded
it, so `bootdev == 0` unconditionally → instant panic regardless of what's
mounted. Seeding `cmos.poke(0x3D, 0x01)` (plus `0x10=0x20` for "drive A: is
1.2MB" and `0x14=0x01` for "a floppy drive is installed", the equipment
byte) immediately changed the banner's last line from `FATAL: No bootable
device.` to **`Booting from Floppy...`** — the BIOS reads our FDC, accepts
the media, and proceeds into the actual boot attempt. Execution continues
correctly afterward (no crash, no unimplemented-opcode hits) but hadn't yet
reached the boot sector's own code (still inside system-BIOS segment F000
when this investigation had to stop for time) — worth a few more minutes
of the same tracing approach to confirm the INT 13h read + far jump to
`0000:7C00` next session, but the hard part (a real bootable disk, real
BIOS, and the exact CMOS bytes it needs) is now done and verified.

**Wired into the permanent build**: `Machine::configure_factory_cmos()`
(called once, automatically, by `Machine`'s constructor) now seeds exactly
these bytes -- base memory size, floppy types, equipment byte, boot
sequence, and a correctly-computed checksum -- matching how a real AT's
Setup diskette would have written them once, at the factory, before the
machine ever shipped. Covered by
`MachineTest.ConstructorSeedsFactoryCmosConfiguration`.

## 9. Two more real bugs found and fixed -- reached genuine boot-sector and kernel code

Wiring the factory CMOS config into `Machine` immediately surfaced a real
bug: **CMOS is battery-backed and must survive any reset**, exactly like
`mem`/`rom_`/mounted floppy media already do in this codebase -- but
`Chipset::reset()` was unconditionally calling `cmos.reset()` (and, for the
same reason, clearing `rom_`'s write-protection flags too). The moment any
caller did the natural thing -- construct a `Machine`, then call
`m.reset()` -- the freshly-seeded factory configuration was silently wiped
back to zero. Fixed by removing `cmos.reset()` (and the `rom_` clear) from
`Chipset::reset()` entirely; regression-tested
(`MachineTest.FactoryCmosSurvivesAnExplicitResetCall`).

With that fixed, the BIOS reached "Booting from Floppy..." via the
permanent build, but then got stuck in an apparent infinite loop right at
the point of processing the floppy's completion. **Switched investigative
method here** rather than continuing to hand-decode opcode bytes: installed
Python's `capstone` disassembler and read the actual mnemonics of the stuck
region instead of guessing from hex. That immediately revealed the real
shape of the code -- it's the floppy IRQ6 handler (`int0Eh`), and it
legitimately has *two* paths: if the FDC's Main Status Register doesn't
yet show a completed result phase, it proactively issues its own SENSE
INTERRUPT STATUS and drains it; but if a result phase *is* already ready,
it takes a different, shorter path that just EOIs the PIC, sets a
BIOS-data-area completion flag, and returns -- deliberately leaving the
FDC's actual result bytes for **foreground** code to drain later. That's
completely legitimate real BIOS design.

The bug was in `chipset.cpp`: **IRQ6 was modeled as level-triggered
(`if (fdc.irq_pending()) pic_master.raise(6);` called every tick), when
every real ISA interrupt is edge-triggered** -- it fires once, on the
transition, not continuously for as long as the underlying condition
holds. Since the "short path" ISR doesn't drain any result bytes,
`fdc.irq_pending()` legitimately stays true for a while (real foreground
code is supposed to drain it later) -- and re-raising IRQ6 every single
tick while it waited turned one real, single interrupt into an infinite
storm that re-entered the ISR every ~42 cycles forever, permanently
starving the foreground code of any chance to run at all. Fixed by
tracking the 0→1 transition explicitly (`fdc_irq_prev_`) and raising IRQ6
only on that edge, matching genuine 8259/ISA wiring. Regression-tested at
the chipset level (`ChipsetTest.Irq6IsEdgeTriggeredNotReRaisedWhileStillPending`),
reproducing the exact "service once without draining, verify no re-trigger"
scenario.

**Result**: with both fixes, the BIOS loads and executes the **real boot
sector** at `0000:7C00` and continues into real FreeDOS kernel code,
reaching a `HLT`-based wait. Chasing what that wait was blocked on (§10,
below) led to one more real bug and then a complete, successful boot.

## 10. Full success: a live, interactive `A:\>` DOS prompt

**Switched method again**: installed Python's `capstone` disassembler
(already in use for §9) to read the *live RAM* the halted CPU was sitting
in, not just the ROM. That immediately answered the "is this a crash or a
wait" question: `0000:7C00` held genuine, structured x86 (the real boot
sector, not blank memory), and the halt itself was sitting inside a
completely ordinary DOS idiom --- `INT 2Fh AX=1680h` ("release current
virtual-machine time slice", the standard DOS-idle multiplex call used by
TSRs/multitaskers) followed by `HLT`, looping until a shared flag byte
exceeded 1. Dumping the **EGA text VRAM directly** (rather than relying on
the `wrch()` character log, which only ever sees the system BIOS's own
print helper, never anything DOS itself prints via `INT 10h`) confirmed
it immediately: the screen held a complete, real FreeDOS 1.3 installer
screen --- ASCII-art logo, welcome text, and `Do you want to proceed
[Y,N]?` --- genuinely waiting for a keystroke.

Injecting one (`chipset.kbc.inject_scancode()`) to test the input path
surfaced one more real, previously-undiscovered bug: **IRQ1 (the keyboard)
had never been wired to the PIC at all.** `chipset.cpp` handled IRQ0 (PIT)
and IRQ6 (FDC) but simply never checked `kbc.irq1_pending()` -- a
plain omission, not a subtle timing bug like the other two. A keypress sat
in `i8042`'s output buffer forever; anything waiting on the *vectored*
keyboard interrupt (rather than polling port 0x60 directly) could never
wake. Fixed the same way as IRQ6 (`chipset.cpp` raises IRQ1 only on the
0->1 edge of `kbc.irq1_pending()`) plus one more real detail: real
hardware ties IRQ1 directly to the "output buffer full" condition, so
`i8042::in(0x60)` now clears `irq1_pending_` the moment the byte is read,
matching genuine wiring. Regression-tested
(`ChipsetTest.KeyboardIrq1ReachesThePic`).

With that fixed, injecting `N` made FreeDOS **echo it to the screen**
(`Do you want to proceed [Y,N]?N`) -- proof the full input round trip
works: scancode -> IRQ1 -> PIC -> CPU -> the system BIOS's real `INT 9h`
handler -> the BIOS keyboard buffer -> FreeDOS's own input routine -> a
character drawn to EGA VRAM by FreeDOS's own code, not a stub. Following
with `Enter` completed the prompt:

```
The installation of FreeDOS 1.3 has been aborted.
A:\>
```

**A live, interactive DOS prompt.** `CS:IP` at that point is `06B3:1570`
with the CPU *not* halted -- COMMAND.COM's own input loop, actively
running, waiting for the next command. This is a genuine, unmodified
`BIOS-bochs-legacy` + unmodified `vgabios` + genuine FreeDOS 1.3 kernel +
FreeCOM, booted from a real 1.2MB floppy image, with full keyboard input
and video output working end-to-end -- not a stub, not a mock, the actual
software doing what it does on real hardware.

**Every bug found this session (three: `0x66` operand-size coverage, the
IRQ6 edge-vs-level modeling, the missing IRQ1 wire) was found through
disassembly/RAM-inspection evidence against real third-party software,
never guessed, and each has a regression test.** 117/117 tests pass.
Phase 3's original goal -- floppy + EGA text mode + booting to a prompt --
is not just met, it's demonstrated with real, unmodified software.

## 11. Phase 4: WD1003 hard disk controller

Read the real `rombios.c` (Bochs BIOS, pinned commit
`ff17a0c2bbabccf96d33af4e08ba8061889b079d`) *before* writing any of
`wd1003.h`/`.cpp`, specifically `ata_detect()` and `hard_drive_post`, to
avoid the multi-round trial-and-error Phase 3's FDC work needed. That
research settled three design questions the approved plan had explicitly
deferred:

**Detection protocol is genuine ATA/IDE, not the classic CMOS type-table.**
`ata_detect()` writes a `0x55`/`0xAA` scratch pattern to the Sector
Count/Number registers (0x1F2/0x1F3) and reads it back to confirm something
answers, issues a soft reset via the Device Control register's SRST bit
(port 0x3F6) and checks the documented post-reset signature (sector
count/number = 1, cylinder = 0), then issues `IDENTIFY DEVICE` (`0xEC`) and
reads drive geometry directly out of the response -- word 1 = cylinders,
word 3 = heads, word 6 = sectors/track, words 60-61 = total LBA sector
count. `wd1003.cpp`'s `do_identify()` implements exactly this layout
(including the ATA string-field convention of byte-swapping character pairs
within each 16-bit word, used for the model/serial/firmware strings).

**The legacy CMOS "Type 47" fixed-disk table is a separate, coexisting path.**
`hard_drive_post` *also* reads a completely different legacy structure --
CMOS register 0x12's high nibble must read `0xF` ("use extended type") or
the whole block is skipped, then register 0x19 must read exactly 47, and
registers 0x1B-0x23 hold cylinders/heads/write-precomp/control-byte/landing-
zone/sectors-per-track, filled by `hard_drive_post` into a legacy EBDA
parameter table for `INT 41h`/`INT 46h` callers. This doesn't replace the
ATA/IDENTIFY path -- both run, both must describe the same drive, or the two
would disagree with each other. `Machine::configure_factory_cmos()` seeds
both, describing the identical ST-4038 geometry (733 cyl / 5 heads / 17
sec/track, no write precomp, landing zone 733) in each place.

**Real port wiring: 0x3F6, not 0x3F7.** A genuine AT's hard disk control
block uses only 0x1F0-0x1F7 plus 0x3F6 (Device Control / Alternate Status)
-- not 0x3F7, which the floppy controller already owns (DIR on read, CCR on
write). `wd1003.h`'s header comment records this as a deliberate period-
accurate wiring decision, not an arbitrary port choice.

**Genuinely atomic 16-bit port I/O.** Unlike almost every other AT I/O
device, where a 16-bit access legitimately decomposes into two adjacent
8-bit ports, the ATA data register at 0x1F0 is atomically 16-bit -- 0x1F1 is
a *different* register (Error/Features), not "the high byte of 0x1F0".
`cpu80286.h`'s `Bus` gained `in16`/`out16` alongside the existing 8-bit
`in`/`out`, used by the four 16-bit port opcodes (`IN AX,imm8`/`OUT
imm8,AX`/`IN AX,DX`/`OUT DX,AX`) and `INSW`/`OUTSW`;
`Chipset::io_in16`/`io_out16` special-case port 0x1F0 to reach
`Wd1003::data_in16()`/`data_out16()` directly, defaulting to the old
compose-two-8-bit-accesses behavior everywhere else. Regression-tested at
the CPU level (`Cpu80286Test.{In,Out}{Ax,Dx}*AtomicSixteenBitPath`,
`InswStoresAtomicSixteenBitPortReadAtEsDi`,
`OutswSendsAtomicSixteenBitPortWriteFromDsSi`) and end-to-end through the
chipset (`ChipsetTest.HddIdentifyAndReadGoesThroughAtomicSixteenBitBusPath`).

**A genuine port-conflict bug, caught by the first chipset-level integration
test.** `Fdc765::owns()` claimed the whole `0x3F0-0x3F7` range -- wrong:
0x3F6, sitting in the middle of that otherwise-contiguous-looking block,
belongs to the hard disk controller, not the floppy controller, on real
hardware. Because `Chipset::io_in`/`io_out` check `fdc.owns(port)` before
`hdd.owns(port)`, every access to 0x3F6 was silently being routed to the FDC
(which has no case for it and falls through to open-bus/discard) and never
reached the HDD at all -- caught immediately by
`ChipsetTest.HddIdentifyAndReadGoesThroughAtomicSixteenBitBusPath` returning
garbage instead of the expected sector data. Fixed by narrowing
`Fdc765::owns()` to `0x3F0-0x3F5` and `0x3F7` only, matching what
`fdc765.cpp`'s own `in()`/`out()` switch statements actually implement.
Regression-tested directly (`Fdc765Test.DoesNotOwnPortThreeF6`) as well as
via the now-passing HDD integration test.

**Byte-credit pacing, same discipline as `fdc765.h`.** READ/WRITE SECTORS
accumulate transfer time against a real bytes/sec rate (raised BSY, `tick()`
completes the whole bulk copy once elapsed) rather than modeling byte-by-
byte hardware handshake -- WRITE SECTORS asserts DRQ immediately per real
ATA behavior (no seek delay on the CPU side) and only raises BSY once the
CPU has supplied the full buffer, pacing the *commit* to media rather than
the CPU's data delivery.

**Edge-triggered IRQ14**, on the slave PIC's line 6 (global IRQ14 = slave
IR6), following the exact discipline already established for IRQ6/IRQ1 (see
§8-9): raised only on the 0->1 transition of `hdd.irq_pending()`, and
reading the primary Status register (0x1F7) acknowledges/clears it, matching
genuine ATA semantics -- reading the Alternate Status register (0x3F6)
deliberately does not, so a test (or a real driver) can poll status without
disturbing a pending interrupt. Regression-tested
(`ChipsetTest.Irq14IsEdgeTriggeredNotReRaisedWhilePending`).

All of this is implemented and unit-tested (13 new/changed cases across
`wd1003_test.cpp`, `chipset_test.cpp`, `fdc765_test.cpp`, and
`cpu80286_test.cpp`; 133/133 total tests pass) purely at the device/chipset
level, driving the controller directly rather than through a booted BIOS.

**Decision: build the shipped HDD image by actually running the real
installer, not by hand-crafting a filesystem.** No pre-built 733/5/17-
geometry raw HDD image exists (unlike the floppy, which is an official
pre-built FreeDOS distribution file). Rather than synthesize a FAT
filesystem directly, the shipped image is produced by driving the genuine
FreeDOS 1.3 multi-floppy installer (`FD13-FloppyEdition.zip`'s `BOOT` +
`DSK01`-`DSK06`, the real installer set, not the pre-built boot floppy
alone) against this emulator end-to-end -- the same unmodified BIOS,
VGABIOS, and installer software a real 5170-339 owner would have used,
automated via screen-text matching and injected keystrokes instead of a
human at the keyboard. This produces a genuinely-installed disk, not an
approximation of one, and is "as realistic as FreeDOS allows" per the
instruction that scoped this work.

**Three more real bugs, found the same way as Phases 2-3: booting real,
unmodified software and reading the actual BIOS source when behavior
didn't make sense, never guessing.**

1. **A phantom second drive caused a genuine BIOS divide-by-zero.**
   `Wd1003` modeled a single shared task-file register set regardless of
   which drive was selected. Since drive 1 (the slave/D: position) is
   permanently unpopulated on this machine, selecting it should make the
   command-block bus float (real absent ATA devices don't drive the bus at
   all), but the shared-register model just echoed drive 0's real values
   back. The BIOS's own `ata_detect()` (`rombios.c`) scratch-register test
   (write 0x55/0xAA to Sector Count/Number, read it back) therefore
   concluded a second drive existed, IDENTIFYing it returned all-zero
   geometry (nothing was ever mounted at index 1), and the BIOS's extended-
   geometry query (`AH=48`) divided by that zero somewhere in its own CHS-
   translation math -- a hard `#DE` inside the BIOS itself (confirmed by
   instrumenting `Cpu::interrupt()` and disassembling at the faulting
   `CS:IP`), which the FreeDOS kernel's own exception handler then reported
   as `PANIC: MCB chain corrupted`. Fixed by making register access for a
   permanently-absent drive index float to `0xFF` on read and go nowhere on
   write (`Wd1003::selected_drive_absent()`), keyed off the fixed hardware
   fact of which drive index is populated -- deliberately NOT off whether
   `mount()` happened to be called yet, since the existing register-
   protocol tests exercise drive 0 without ever mounting an image (matching
   a real controller answering regardless of whether the platters hold a
   valid filesystem). Regression-tested
   (`Wd1003Test.AbsentSlaveDriveFloatsInsteadOfEchoingMasterRegisters`).

2. **Every real disk I/O from this BIOS uses 28-bit LBA addressing, never
   plain CHS -- even for "legacy" `INT 13h AH=02/03` calls.** Reading
   `rombios.c`'s `ata_cmd_data_io()` showed it converts CHS to LBA in
   software for *every* call (the classic sentinel is passing `sector=0`
   to that internal function, which then computes the LBA fields and ORs
   `ATA_CB_DH_LBA` (0x40) into the Drive/Head register) and always issues
   the ATA command that way. `Wd1003` always interpreted Sector
   Number/Cylinder Low/Cylinder High/Drive-Head as pure CHS regardless of
   that bit -- which happened to still work for LBA sector 0 (arithmetically
   identical to CHS(0,0,1) for any geometry, since sector 1 is always the
   first sector) and so stayed hidden through every boot-sector read so
   far, but silently computed the wrong byte offset for anything else. This
   is exactly what broke the FreeDOS installer's auto-partition step, which
   needed to touch sectors where the two addressings genuinely diverge.
   Genuine ATA LBA28 support is the same kind of documented compatibility
   concession as IDENTIFY DEVICE itself (a real 1984 WD1003 predates LBA by
   over a decade) -- implemented in
   `Wd1003::offset_for_current_registers()`, gated on Drive/Head bit 6.
   Regression-tested
   (`Wd1003Test.ReadSectorsUsesLbaAddressingWhenDriveHeadBitSixIsSet`, using
   an LBA address chosen to be out of range under a naive CHS
   interpretation of the same register values, so the test fails loudly if
   the addressing mode is ever ignored again).

3. **A missing IDENTIFY field made the BIOS's own transfer loop move zero
   bytes despite our device having real data ready.** `ata_detect()` reads
   IDENTIFY word 5 ("bytes per unformatted sector" -- an obsolete field on
   any real drive, always 512 in practice) into its own per-device cache,
   then `ata_cmd_data_io()`'s assembly transfer loop sizes its `rep
   insw`/`rep outsw` word count directly from that cached value. `Wd1003`'s
   `do_identify()` never set word 5, defaulting to zero from the buffer's
   initial zero-fill -- so the BIOS's own PIO loop moved zero words per
   chunk. The device's paced READ SECTORS still completed correctly and had
   genuine data sitting behind DRQ the whole time, but the BIOS never
   actually drained any of it, so its own post-transfer completion check
   (which expects DRQ to have cleared once the expected byte count has been
   pulled) saw DRQ still set and reported total operation failure --
   exactly what the installer's "Automatically partition drive C:. Failed."
   message was. Found by tracing the specific `INT 13h AH=42` (extended
   read) call end to end: capturing its Disk Address Packet, confirming
   `Wd1003` itself logged a normal, in-range, successfully-completed read,
   and then reading `ata_cmd_data_io()`'s post-transfer status check in
   `rombios.c` to see exactly what it was waiting for that never happened.
   Fixed by setting IDENTIFY word 5 to 512. Regression-tested
   (`Wd1003Test.IdentifyDeviceReportsRealGeometry`, extended to check word 5).

With all three fixed, the exact real FreeDOS 1.3 installer -- unmodified
BIOS, unmodified VGABIOS, unmodified installer floppies -- now reports
**"Automatically partition drive C:. Success."** against a genuinely blank
733/5/17 image and proceeds to the "reboot to apply the new partition"
step, which itself works end-to-end (a real `INT 19h`-style reboot back
through the same boot floppy, landing at the installer's welcome screen
again with the newly-written MBR now persisted on the mounted image).

**In progress**: scripting the rest of the real installer flow (format,
file copy across the `DSK01`-`DSK06` floppy swaps, completion) via the same
screen-text-matching-and-keystroke-injection approach, to produce the
final genuinely-installed 733/5/17 image; then a pinned/checksummed
`disks/build-freedos-hdd.sh` (fetches `FD13-FloppyEdition.zip`, runs the
installer-automation tool against it, verifies the result boots) following
the repo's fetch-script conventions, and an actual `bios_host`-driven
boot-from-HDD trace against the finished image.

**A fourth real bug, hit as soon as the installer needed to swap floppies
mid-session.** `fdc765.h`'s Digital Input Register (0x3F7) had, since Phase
3, always reported "media unchanged" -- documented at the time as "not
modeled" because nothing yet exercised it (a single-floppy boot never
swaps media at all). Driving the real multi-disk installer past its first
"Insert diskette #2 ... press a key to continue" prompt exposed why that
matters: the installer's own file-copy routine polls this bit specifically
to confirm the user actually swapped media before trusting a re-read of
the drive, and reporting "unchanged" unconditionally left it waiting
forever for a change that could never come -- confirmed by giving it an
enormous cycle budget (20 billion) with zero progress, not just "needs more
time." Fixed with genuine DSKCHG modeling: `Drive::disk_changed` starts
true (real hardware asserts it at power-on), `mount()` re-asserts it
(matching a real media swap), and it clears only once a RECALIBRATE/SEEK
command actually steps that drive afterward (matching a real controller
re-latching the new media) -- `in(0x3F7)` reports it for whichever drive
the DOR's select bits currently address. Regression-tested
(`Fdc765Test.DiskChangeLineSetByMountAndClearedBySeek`).

**Real progress on the multi-disk install, and a fifth real bug in the
scripted keystroke timing.** Extending the disk-swap logic to answer
subsequent "press a key" prompts hit a genuine, permanent `HLT` with
interrupts enabled but never woken -- confirmed with a CS:IP hit
histogram (the same disassembly-based technique as earlier phases)
showing the CPU parked inside the classic `INT 2Fh AX=1680h` DOS-idle
idiom (see §10) with zero PIC pending-interrupt observations across three
million sampled cycles yet real wake events still occurring, which ruled
out an interrupt-delivery bug and pointed at software: the installer's
"press a key" wait apparently flushes/discards whatever's already queued
right before it actually starts waiting, so a burst of presses sent the
instant the prompt's text first appeared can land entirely inside that
flush window and vanish. Fixed by nagging with one keypress every ~0.25s
of emulated time instead of a single burst, matching what an actually
impatient human does. This is a scripting-tool finding, not an emulator
bug -- noted here because the same install-automation approach needed it
to make real progress at all.

**The real file-to-disk mapping, discovered rather than guessed.** Past
the first floppy, the installer names files individually ("Insert diskette
containing file A:\FREEDOS.NNN") instead of by disk number. Parsing each
install floppy's actual FAT12 root directory (rather than guessing) showed
the real split-archive layout: DSK01=FREEDOS.001-019, DSK02=.020-039,
DSK03=.040-059, DSK04=.060-079, DSK05=.080-099, DSK06=.100-114 -- clean,
contiguous ranges, confirming the installer processes files strictly in
numeric order and only asks for a new diskette exactly at a range
boundary.

**With all of the above, the real, unmodified FreeDOS 1.3 installer
completed the entire installation end to end** -- format, MBR, boot
partition, and all 114 split files across all 6 disks -- ending with its
own "The installation of FreeDOS 1.3 has completed" message. The
installer's own in-memory `Wd1003::Drive::image` (not the buffer originally
passed to `mount()`, which only reflects the *starting* state -- a scratch-
tooling detail worth remembering for any future automation, not an
emulator bug) was extracted to a genuine 733/5/17, 31,900,160-byte raw HDD
image.

**Booting that image standalone (no floppy mounted, matching real AT
behavior falling through to drive C: when drive A: is empty) surfaced
three more real bugs, all in how `Wd1003` modeled ATA/IDE's master/slave
bus sharing -- each one only visible because nothing exercises "boot with
zero help from a floppy in between" during a normal install session:**

1. **A real ~32-second hardware timeout, not a bug -- but one that needs
   billions of genuine 8MHz cycles to actually clear.** `rombios.c`'s
   `await_ide()` has no hardware-timer-based timeout at all: `IDE_TIMEOUT`
   (32000, "32 seconds max for IDE ops") is a raw *loop iteration* count,
   calibrated by the BIOS's original authors against whatever host speed
   they had in mind -- at this machine's genuine, unaccelerated 8MHz, the
   real elapsed time to exhaust it is on the order of several real seconds
   of *simulated* clock time, i.e. billions of actual 8MHz cycles. A
   from-cold HDD-only boot legitimately waits out this real timeout for
   the permanently-absent slave drive before POST can continue, and a test
   budget of a few billion cycles simply wasn't enough to observe anything
   past it -- confirmed by disassembling the exact hot loop (an `inb()`
   polling `await_ide` call) and recognizing the calibration, then simply
   giving the harness enough cycles. This is genuine, period-accurate
   behavior worth preserving as-is per this repo's own realism rules, not
   something to special-case away.

2. **The command-register-only write gate was still too broad.** The
   original phantom-drive fix (§ above) gated *every* write aimed at the
   selected-but-absent slave, on the reasoning that "there's no Device 1
   state machine to receive it." True for the command register, but wrong
   for Sector Count/Number/Cylinder Low/High/Features: those are simple
   latches on a shared parallel bus that whichever real device is
   physically present absorbs *regardless of the DEV bit* -- there's no
   per-device routing for them at all on real hardware. This BIOS's own
   boot-sector-read code programs exactly those registers *before*
   reselecting the master (right after probing the slave last during
   POST), so gating them on the then-still-selected slave silently
   discarded the real CHS parameters, corrupting the very first
   boot-sector read into a bogus, negative-offset request. Fixed by
   narrowing the gate to the command register (0x1F7) alone -- found by
   tracing the exact sequence of port writes leading into the failing
   read and noticing Sector Number simply never got programmed.

3. **The absence-signaling fix from bug 2's era was *also* too broad in
   the read direction, in two different ways, uncovered one after the
   other.** First: reads were gated to float (0xFF) whenever the slave
   was selected at all, matching "nothing answers" -- but per the real
   ATA spec, when no Device 1 is wired at all, Device 0 must keep
   answering *reads* too regardless of the DEV bit, since there's nothing
   else on the bus to float to; gating them made the master's own status
   register look permanently floated (BSY stuck set) the instant POST's
   own slave probe left the slave selected, which is exactly the state
   the boot loader's very first status check finds itself in. Un-gating
   all reads fixed that -- but broke detection itself: with reads never
   floating, the slave's post-reset Cylinder Low/High showed the same
   0x00/0x00 signature as a genuine drive, so `ata_detect()`'s own
   `cl==0xff && ch==0xff` "no second drive" check never fired, the slave
   got misclassified as real ATA, and its inevitably-refused IDENTIFY
   attempt made the BIOS panic with "Failed to detect ATA device" after
   timing out. The real, precise fix needed both facts to hold at once:
   Cylinder Low/High/Status float *only* for the exact two reads
   `ata_detect()` performs immediately after a soft reset released while
   the slave was selected (modeled as a two-read countdown,
   `Wd1003::floating_reads_left_`, armed by the reset and consumed by
   the very next two reads of those two registers), and reflect the real,
   present master for absolutely everything else -- including the boot
   loader's later, unrelated status check with the slave still nominally
   selected from that same long-past probe. All three were found via a
   dedicated HDD-only boot verification harness (no floppy mounted at
   all) and a CS:IP hit histogram to pin down exactly where the CPU was
   stuck at each step, the same evidence-based technique used throughout
   this project. Regression-tested
   (`Wd1003Test.AbsentSlaveDriveOnlyRefusesToExecuteCommands`,
   `Wd1003Test.SoftResetSignalsAbsenceForTheSlaveViaFloatedCylinderRegisters`,
   `Wd1003Test.FloatedSignatureIsATwoReadWindowNotAPersistentState`).

**One more real, genuine factory-configuration gap, found by the same
boot test:** `Machine::configure_factory_cmos()` set CMOS byte 0x3D's
1st-boot-device nibble to floppy (0x01) but left the 2nd-boot-device
nibble at 0 ("not defined") -- meaning a from-cold boot with no floppy
present hit the BIOS's own `BX_PANIC("No bootable device.")` rather than
falling through to the hard disk. Real ATs shipped with a hard disk
installed default to exactly "try A: first, then C:" (`rombios.c`'s own
boot-device-code table: 0x01=floppy, 0x02=hard disk, packed low-nibble/
high-nibble into one byte -- 0x21). Fixed by setting CMOS 0x3D to 0x21.
Regression-tested (`MachineTest.ConstructorSeedsFactoryCmosConfiguration`'s
new assertion on the high nibble).

**End-to-end result: a genuinely bootable, from-scratch-installed 30MB
FreeDOS 1.3 hard disk image, produced entirely by real, unmodified
third-party software running inside this emulator -- the real BIOS, the
real VGA BIOS, and the real FreeDOS 1.3 installer, never a hand-crafted
filesystem.** Booting that image standalone (HDD only, no floppy) reaches
a genuine, interactive `C:\>` prompt: `FDCONFIG.SYS`/`FDAUTO.BAT` process,
FreeCOM reports real conventional-memory figures for `SYSTEM` (the
FreeDOS kernel) and `COMMAND` (FreeCOM itself), and the shell's own
welcome banner and prompt appear -- not a stub, not a mock, the actual
software doing exactly what it does on real hardware.

**Done: the scratch install-automation harness became a permanent,
checked-in, reproducible build pipeline.** `disks/build_freedos_hdd.cpp`
is the cleaned-up tool (fixed Step list through the first floppy swap,
then the dynamic per-file/disk-range logic, the keypress-nag mechanism,
and the final-completion handling, all as described above, with every
debug trace this session's investigation needed stripped back out);
`disks/fetch-freedos-install-set.sh` fetches and SHA-256-verifies
`x86DSK01.img`-`x86DSK06.img` from the same official `FD13-FloppyEdition.zip`
distribution point `fetch-freedos.sh` already used for the boot floppy,
following the identical pinned-and-verified pattern. `make hdd-image`
(deliberately *not* part of `check`/`test` -- see the Makefile's own
comment -- since running the real installer takes real minutes) ties it
together: fetch BIOS + both floppy sets, build the tool, run it, and
produce `disks/freedos-hdd.img`. Verified by actually running the full
pipeline from a clean checkout: it reproduced the identical result (a
genuine MBR, `55 AA` signature, and a real, independently-verified boot to
`C:\>`), confirming the shipped HDD image is now something anyone building
this project can regenerate, not an artifact only this session's
interactive debugging could produce. The Makefile's own `CHIPSET_SRCS`/
`UNIT_TESTS` lists and its `coverage` target's source list, silently stale
since Phase 3 (never updated when `fdc765.cpp`/`ega.cpp`/`wd1003.cpp` were
added, so `make bios-check` and `make coverage` had been failing to link
this whole time), were fixed as part of this same pass.

**Still open, for a later phase:** CI integration for `make hdd-image`
(Phase 8's "site integration" scope) -- the real-installer approach that
makes this image genuinely reproducible also makes it too slow to run on
every push; some form of caching keyed on the pinned floppy/tool inputs
will be needed there, not attempted here.

## 12. Phase 5: real planar EGA memory (write modes, read modes, latch)

Phase 3's `ega.h`/`ega.cpp` treated VRAM as a flat, character/attribute-
interleaved byte array -- observably correct for text mode specifically
(a real EGA's odd/even chaining happens to reduce to exactly that for
text), but not the real hardware underneath, and not enough for any
graphics mode. This phase replaces it with the genuine 4-bitplane engine:
256KB VRAM stored byte-interleaved as `vram[(plane_offset << 2) + plane]`,
a 4-byte read latch, the full Write Mode 0-3 / Read Mode 0-1 state machine,
Set/Reset, Enable Set/Reset, Data Rotate's rotate-count + ALU function,
Map Mask, Read Map Select, Bit Mask, Color Compare/Color Don't Care, and
the Graphics Controller's Memory Mapping field (which legacy window --
128K@A0000, 64K@A0000, 32K@B0000 mono, or 32K@B8000 color -- is currently
decoded; real hardware only ever answers one at a time, not all three
simultaneously the way Phase 3's flat model implicitly did).

**Source**: the memory-access algorithm (latch-on-every-read, the four
write modes, both read modes, the odd/even-vs-map-mask gating) matches
Bochs's own reference implementation, `bx_vgacore_c::mem_read`/`mem_write`
in `vgacore.cc`, at the same pinned commit
(`ff17a0c2bbabccf96d33af4e08ba8061889b079d`) this machine's own BIOS and
VGABIOS images are built from -- fetched and read directly (`curl` + a
plain read, not `WebFetch`, which repeatedly either failed outright on
other EGA/VGA reference pages -- an expired cert on osdever.net, HTTP 403
on osdev.org's wiki, `web.archive.org` refusing entirely -- or, for
`vgacore.cc` itself, returned a lossy model-summarized paraphrase instead
of exact register semantics; this is the same "fetch the real source
directly" discipline already established for `rombios.c`). Register
*values* (the standard mode-3 and 16-color-graphics-mode table used in
`tests/ega_test.cpp`'s setup helpers) are the IBM EGA/VGA-standard values
every compatible BIOS reproduces for exact hardware compatibility, and were
additionally confirmed empirically: the regression check below booted the
real, unmodified `vgabios` and it produced byte-for-byte the same screen
output as Phase 3's simplified model, meaning vgabios's own mode-3 register
programming matches what this implementation expects.

**Explicitly out of scope: Chain Four (VGA mode 13h's chained-pixel
addressing).** `VGABIOS-lgpl-latest.bin` is a full VGA-compatible BIOS (see
§6) and will happily offer mode 13h, but Chain Four is VGA silicon, not
EGA -- a genuine 5170-339 with an EGA card has no such mode. This device
does not implement it, on purpose, matching CLAUDE.md's "Adding a new
machine" rule that the emulated hardware -- not whatever the substitute
BIOS thinks it can offer -- is what enforces the real EGA ceiling
(640x350x16, no chained/linear-framebuffer modes).

**Regression risk and how it was checked**: this is a rewrite of the one
piece of hardware the entire, already-verified boot pipeline (Phases 2-4)
depends on for any visible output at all, so it was checked two ways
before being considered done, not just unit-tested in isolation:

- The 6 Phase-3 `ega_test.cpp` cases were kept and still pass, but 2 of
  them needed real changes, not just adaptation: `TextModeMemoryReadWriteRoundTrip`
  now calls a `SetupTextMode80x25()` helper first, because a freshly reset
  card genuinely has no planes enabled and an all-zero Bit Mask -- on real
  hardware a bare `mem_write` right after reset, with no mode-set, *is* a
  no-op, so the old test's implicit assumption otherwise was itself a
  Phase-3 simplification artifact, not something to preserve.
  `GraphicsAndMonoWindowsAreDistinctFromColorTextWindow` similarly assumed
  all three legacy windows (A0000/B0000/B8000) are simultaneously live,
  which no real register state produces; it's replaced by
  `MemoryMappingSelectsWhichLegacyWindowIsDecoded`, which demonstrates the
  real behavior (switching the Memory Mapping field changes which one
  window answers, and the previous window's bytes are preserved but no
  longer reachable, not aliased). 6 new tests cover the write-mode/read-
  mode/Set-Reset/Data-Rotate/Map-Mask/Bit-Mask machinery and the odd/even-
  routed character-generator-plane-2 quirk individually.
- A scratch native harness (not committed -- same shape as
  `disks/build_freedos_hdd.cpp`'s screen-reconstruction technique) booted
  the real `BIOS-bochs-legacy` + `VGABIOS-lgpl-latest.bin` against the
  shipped `disks/freedos-hdd.img` end to end and reached the identical live
  `C:\>` prompt Phase 4 established, with the identical on-screen text
  (memory-usage table, `FDCONFIG.SYS`/`FDAUTO.BAT` processing, the FreeDOS
  banner) -- proving the new planar engine is observably indistinguishable
  from the old flat model for everything real firmware/DOS currently
  exercises. (The harness's own screen-reconstruction code needed a
  matching fix along the way: it originally read `vram[]` directly with
  Phase 3's flat offsets, which no longer describes the storage layout --
  fixed to fold the CRT controller's own display-refresh read path,
  character = plane 0 at `vram[(plane_offset<<2)+0]`, mirroring real
  hardware's independent CRTC scanout path rather than the CPU's I/O read
  path (`mem_read`), which depends on Read Map Select and would give a
  wrong answer if a BIOS font-load routine left it pointed elsewhere.)

**Deferred to Phase 7** (per the approved plan, matching this project's
"native/testable core first, WASM front end later" pattern from every
prior phase): the `<canvas>`-based renderer that actually paints EGA's
character/graphics modes to the screen. This phase's scope is the real
device semantics only.

**`render_screen.cpp`** (promoted from a scratch check into a permanent
diagnostic, same category as `bios_host.cpp`): boots the real BIOS/vgabios
plus optional HDD/floppy images, then renders the live EGA text-mode
screen to an actual BMP -- not the ASCII `ScreenText()` reconstruction
`disks/build_freedos_hdd.cpp` uses for its own keystroke-matching, which
is fine for pattern-matching prompts but tells a person nothing about
whether the *real* character generator and palette are actually correct.
It uses only already-public `Ega` state (`vram` for both plane 0/1 text
data and plane 2's character-generator bitmap; a new `attr_palette()`
accessor, mirroring the existing `cursor_offset()`/`start_offset()`
front-end-convenience pattern, added alongside it for the live Attribute
Controller palette registers) plus the same 6-bit EGA color decode cited
in this section -- no separate font or color data of its own. Verified by
actually running it against the shipped `disks/freedos-hdd.img`: produced
a real, legible, correctly-colored screenshot of the live `C:\>` boot
(readable glyphs confirm the plane-2 character-generator addressing
assumption; correct colors -- cyan filenames, red CD-ROM warning, green
FreeDOS banner -- confirm the default palette register table and 6-bit
color decode), independent visual proof beyond the ASCII-based regression
check above. Not gated by `check`/`test`, matching `bios_host.cpp`'s own
precedent -- see the Makefile's comment on `render_screen`.
