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

## 13. Phase 6: PC speaker

`pcspeaker.h`/`.cpp` model the real speaker circuit as what it actually is
in hardware: a 2-input AND gate between Port 0x61 bit 1 ("Speaker Data
Enable") and PIT channel 2's output (already gated by Port 0x61 bit 0 --
see `pit8253.h`/§5). That one AND gate is why two unrelated real
programming techniques both work through the same two bits: standard tone
generation (gate the PIT on, leave data enable high, the speaker follows
channel 2's square wave) and "digitized"/direct-toggle playback (park the
PIT -- gate off, forcing its output permanently high -- and toggle data
enable directly under CPU control, e.g. Access Software's RealSound and
many disk-based PC speaker sample players). `PcSpeaker` doesn't synthesize
audio itself (no browser exists yet -- Web Audio output is Phase 7's job,
per the approved plan); it records a real, continuous *edge trace*
((cpu_cycle, level) whenever the AND gate's output actually changes), the
representation a future renderer needs to resample into PCM, and the only
one faithful to a real speaker's continuously-variable cone position
rather than some sample rate the native core has no business choosing.
Bounded to 65536 pending edges (real hardware has no such limit; this only
protects memory if a consumer never drains, dropping the oldest
transitions the way an unread hardware FIFO would).

**Two real `Pit8253` gaps found and fixed along the way**, both load-
bearing for the digitized-playback technique specifically (nothing before
Phase 6 needed to gate channel 2 off mid-tone, so nothing exposed them):
gate low was freezing channel 2's counter correctly but leaving `output`
at whatever level it happened to be, when real Mode 3 hardware forces it
high immediately regardless of phase -- without this, a program parking
the PIT to drive the speaker directly would inherit an unpredictable
baseline instead of the clean high level real hardware guarantees. And
gate's rising edge wasn't reloading the counter, when real Mode 3 hardware
does -- without this, ungating would resume mid-phase instead of
restarting the square wave cleanly. Both are cited, hand-verified 8253
Mode 3 behavior (see `pit8253.h`'s updated header), covered by two new
tests (`Gate2LowForcesOutputHighEvenMidCycle`,
`Gate2RisingEdgeReloadsCounterInsteadOfResumingMidPhase`) that fail
without the fix and pass with it -- confirmed by deriving both by hand
before implementing, then checking the actual test run matched.

**Verified two ways**: `pcspeaker_test.cpp`'s 6 cases exercise the AND
gate, edge deduplication, the digitized-playback relay case, reset, and
the overflow-drops-oldest behavior directly. And the full BIOS + vgabios +
`freedos-hdd.img` boot (the same regression check used for Phase 5) was
re-run after wiring `PcSpeaker` into `Chipset` and fixing the two PIT
gaps: identical screen output, confirming no regression. That same run
also traced Port 0x61 across the entire boot: it never changes from
0x00, so the speaker produces zero edges for this specific boot path --
not a bug (confirmed by tracing the actual register, not just the
higher-level symptom): this substitute BIOS's boot sequence and a plain,
silent `C:\>` boot apparently never call for a beep. The device logic
itself is exercised directly by its own unit tests instead.

**Deferred to Phase 7**: actually turning the edge trace into sound (Web
Audio, matching the approved plan's "Web Audio output" phrase for this
phase, and this project's native-core-first pattern from every prior
phase). This phase's scope is the real circuit semantics only.

## 14. Phase 7: the front end -- `web/`

`retroweb/ibmpc-at/web/` follows the established per-machine shape
(`wasm_machine.cpp` Embind wrapper, `app.js`/`index.html`, own `Makefile`/
`devserve.py`, independent theme-system copy -- see the approved plan's
"front-end code... keep independent per-machine copies" decision). The
top-level `Makefile` gained `wasm`/`serve` targets delegating to `web/`,
matching `altair8800`'s and `cg-oac-6502`'s own top-level Makefiles
exactly.

**Scope decision, stated up front**: this pass ships real, complete
**text-mode** rendering (what the machine actually boots to and what a
plain DOS session is), keyboard input, the two floppy bays, a status-only
HDD LED, and a muted-by-default Web Audio speaker. CRTC-timing-driven
**graphics-mode** scanout -- deriving the active resolution from the
Horizontal/Vertical Display End registers for 320x200x16/640x350x16/etc.,
which Phase 5's memory engine already fully supports at the device level
-- is deliberately not attempted here; nothing about the plan's Phase 7
line ("full EGA... a new canvas-based front-end renderer") specifies
graphics modes must land in the same pass as text mode, and shipping a
half-working graphics renderer would be worse than being explicit that
it's still missing. A future pass extends `ega_render.h`'s single
`RenderTextScreen` entry point with a second, CRTC-driven one.

**wasm_machine.cpp**: a thin `WasmMachine` wrapping `ibmpcat::Machine`,
exposing `loadRom`/`runCycles`/`renderFrame`/`injectScancode`/floppy
mount-eject-status/`mountHdd`/`hddBusy`/`speakerLevel`+`speakerEdges`.
`renderFrame` calls the same `ega_render.h::RenderTextScreen` the native
`render_screen.cpp` diagnostic uses -- one tested C++ implementation, not
a second copy of the pixel-decode logic re-derived in JavaScript.
Building it surfaced one real, small gap needing new accessors: nothing
exposed the WD1003's busy/DRQ state for an activity LED, so `Wd1003::busy()`
was added (`ST_BSY` or an in-flight paced transfer -- see `wd1003.h`),
covered by a new `BusyReflectsAnInFlightTransfer` test.

**Firmware/HDD image placement**: the deployed site only ever stages a
machine's `web/` directory (see CLAUDE.md/the other machines' own
`_site/` staging), so `web/Makefile`'s `roms`/`hdd-image` targets copy
the native-core Makefile's already-fetched/-built output into `web/roms/`,
`web/disks/` rather than the page fetching `../roms/...` at runtime (which
only works from a working tree, never from the deployed static site).
`hdd-image`'s copy is gated on the parent's own output file actually being
missing (a real file-based Make dependency, not a phony target re-invoked
on every `make serve`) -- unlike `bios`'s recipe, which is cheap/idempotent
(`fetch-bios.sh` checksums and skips what's already correct) and safe to
re-run unconditionally, `hdd-image`'s recipe is a genuine multi-minute
installer run and must not fire by accident.

**Keyboard**: `app.js`'s `SET1` table maps `KeyboardEvent.code` directly
to real IBM AT Set 1 scan codes (a plain number = one byte; a two-entry
array = an 0xE0-prefixed extended key -- the arrow cluster, Insert/
Delete/Home/End/PageUp/PageDown, right Ctrl/Alt, numpad Enter/Divide).
`i8042.h`'s `inject_scancode()` is a verbatim Set-1 pass-through (see §12's
commit and the file's own header), so this table supplies exactly what a
real AT keyboard's own Set-2-to-Set-1 translation would hand the host --
no separate translation stage needed or modeled.

**A real, hardware-accurate constraint this surfaced, not a bug**: the
8042's output register is a single, unqueued byte (`I8042::push_output()`
just overwrites it) -- genuine hardware behavior, not a simplification.
Sending a key's make code immediately followed by its break code with *no*
CPU execution in between overwrites the make code before the CPU ever
reads it, exactly like a real keyboard controller would drop a byte no
one serviced in time. This was caught the hard way: an early Playwright
smoke check using `keyboard.type()`/`keyboard.press()` with their default
near-zero inter-event delay produced no visible effect at all, while the
exact same check with a realistic ~80ms delay between keydown and keyup
(matching how fast a real key can physically be pressed and released, and
how a real browser's own native keyboard events behave) worked perfectly
end to end -- typed `dir`, pressed Enter, got the real FreeDOS directory
listing, screenshotted. **Phase 8's Playwright suite must use a delay
(or `disks/build_freedos_hdd.cpp`'s own proven `SendKey()` shape: make,
run real cycles, break, run real cycles) for synthetic keystrokes** -- this
isn't a workaround for a bug, it's what real hardware requires too, and a
zero-delay synthetic key event is not something any physical keyboard
could ever generate.

**Floppy bays**: functional, theme-consistent panels (file input to
mount, per-drive motor LED polled each frame via `floppyPresent`/
`floppyMotorOn`, an Eject that offers the modified image back as a
download via `floppyImage`/`floppyDirty` when the session actually wrote
to it) -- not a hand-crafted photoreal drive cabinet like `altair8800`'s
88-DCDD graphic. A visually elaborate bay is a nice-to-have follow-up, not
load-bearing for a correctly-emulated machine, and wasn't where this
pass's effort went.

**PC speaker, muted by default (explicit user instruction)**: the
`#speakerEnabled` checkbox starts unchecked on every page load and is
never restored from a saved preference -- deliberately, not merely
because browsers block audio autoplay until a user gesture (though they
do): the point of "off by default" is that it stays off until the visitor
explicitly opts back in, on every visit, not just the first. When enabled,
`pumpAudio()` drains `speakerEdges()` once per animation frame and builds
one `AudioBuffer` covering exactly that frame's CPU cycles from the real
edge trace (holding the last known level between edges, via the new
`speakerLevel()` accessor for frames with none), scheduled gapless via
`AudioBufferSourceNode.start(nextPlayTime)` -- no waveform assumption of
its own, no deprecated `ScriptProcessorNode`, no separate `AudioWorklet`
module. Not audible-output-verified in this pass (this specific boot
path produces zero speaker edges at all, per §13 -- there's nothing to
hear yet without a program that touches Port 0x61); the underlying
`PcSpeaker` device logic has its own 6 direct unit tests, and the
scheduling code is simple, low-risk glue. A real "does it actually make a
sound" check is Phase 8 work, once a test program that beeps exists.

**Verified**: `161/161` native tests pass (new:
`Wd1003Test.BusyReflectsAnInFlightTransfer`). The front end was verified
with ad hoc headless-Chromium (Playwright) checks -- not yet the formal
Phase 8 suite, but real, running-browser proof, not just "it compiles":
default-muted speaker checkbox confirmed unchecked; a full boot from cold
start to a live `C:\>` prompt, screenshotted, byte-for-byte the same
banner/memory-table/FreeDOS-welcome text as every native regression check
in §12/§13; realistic-speed keyboard input typing `dir` + Enter and
getting the real FreeDOS directory listing back, screenshotted; theme
switching (Dark Modern correctly sets `data-theme="modern"
data-mode="dark"`); floppy mount (label updates to the real filename) and
eject (label reverts, motor LED reflects `floppyMotorOn`); the speaker
checkbox toggling on. A `window.__test` hook (`{ machine, sendKey,
screenEl }`, active under `?test=1`) was added for this, matching
`altair8800`/`cg-oac-6502`'s own established `window.__test` convention --
ready for Phase 8's real suite to build on rather than reinvent.

**Deferred to Phase 8**, per the approved plan: the formal Playwright
suite (one spec per control/feature, shared `helpers.ts`/`fixtures.ts`),
top-level `retroweb/Makefile`/`README.md`/`index.html` integration, and
the CI job pair. `?test=1` forcing floppy/HDD load speed to an internal
max (CLAUDE.md's sanctioned automated-test override) is not yet wired up
-- today's ~45-50 second real-time boot (genuine, unaccelerated 8 MHz --
never sped up, per CLAUDE.md) is exactly as slow in the browser as in the
native regression harness, and a test suite will want that override the
way `fdc765`/`wd1003`'s existing credit-based transfer pacing already
supports internally.

## 15. CGA-compatible 4-color graphics mode rendering

Found via real-world use, not a synthetic test: loading an actual
commercial DOS game (The Oregon Trail, MECC, 1990 -- a legitimately-owned
copy, built into a real FAT12 floppy image via `mtools` for testing, not
committed to this repo) and running `OREGON CGA` produced a garbled field
of colored blocks instead of the game's menu. Diagnosed with this
project's standard evidence-based method -- not a guess: a native scratch
harness booted the same way, launched the game, and dumped the actual
Graphics Controller/Sequencer registers afterward. Confirmed precisely:
`GR06` bit 0 (graphics, not alphanumeric) set, `GR05` bits 5-6 (Shift
Register field) = 1 -- the real EGA/VGA "CGA-compatibility" 320x200
4-color mode (what INT 10h mode 4/5 programs), not a bug in Phase 5's
memory engine. §14's `RenderTextScreen` -- correctly scoped as text-only
at the time -- was simply being asked to decode graphics VRAM as if it
were characters and attributes; that's the garbled blocks, not a real or
new emulation defect.

**The real hardware mechanism, worked out from the actual register
values and Phase 5's already-correct odd/even plane chaining** (see
`ega_render.h`'s file header for the full account): a CGA-unaware
program writes what it thinks is one flat 8000-byte bank (even scanlines
in the first 8K, odd in the second, 80 bytes/scanline, 4 pixels/byte).
Odd/even plane chaining -- already implemented, unmodified -- splits
consecutive bytes across planes 0 and 1 by address parity, so each
CGA-style byte pair lands at the same plane offset, one byte per plane.
Real hardware's shift registers then read plane 0's byte as pixels 0-3
and plane 1's byte as pixels 4-7 of that pair, each 2-bit value indexing
the *live Attribute Controller palette* (registers 0-3) -- not a
hardcoded CGA color table, matching how text mode's colors already work.

**Implementation**: `ega.h` gained two small accessors matching the
existing "host/front-end convenience" pattern (`attr_palette`,
`cursor_disabled`, etc.) -- `graphics_mode_active()`, `gc_shift_register_mode()`
-- reading straight from the same register arrays `in()`/`out()` already
use, no new state. `ega_render.h`/`.cpp` gained `DetectScreenMode()`
(reads those two registers, exactly what a real CRT controller consults,
not a BIOS-video-mode-number guess), `RenderCgaGraphics4Screen()`, a
`RenderedFrame{width,height,rgba}` struct, and `RenderScreen()` -- the one
entry point `render_screen.cpp` and `wasm_machine.cpp` both now call, so
there's a single tested implementation deciding what's on screen rather
than each caller guessing or duplicating logic. An unrecognized graphics
mode (Shift Register value 0 -- native 16-color EGA, needing full
CRTC-timing-derived resolution, still not implemented; or 2, a VGA-only
variant no genuine EGA ever sets) renders as a plain black frame -- an
honest "not yet supported" placeholder, not a repeat of the
garbled-block bug for a different unimplemented mode.

**Front end**: `wasm_machine.cpp`'s `renderFrame`/`renderWidth`/
`renderHeight` switched to `RenderScreen`/`RenderedFrame` (resolution
varies by mode now, so width/height are read after each `renderFrame()`
call, not assumed fixed). `app.js` resizes the canvas element's own
pixel buffer to match whenever it changes, letting it display at its own
native aspect ratio rather than stretching a lower-resolution mode into
the text mode's box -- there's no real hardware basis to prefer one
distortion over another, so this doesn't introduce one.

**Also added while diagnosing this** (the user asked directly): real
activity-LED behavior for the floppy motor and HDD indicators, not just
existence. Sampling `busy()`/`motor_on` once per animation frame, *after*
running that frame's whole cycle budget, could miss activity entirely --
a transfer can start and finish well within one frame's ~133,000 cycles.
`WasmMachine::runCycles()` now sub-chunks (2000 cycles at a time -- cheap;
this emulator already interprets one instruction at a time in C++, a far
finer granularity) and latches activity along the way; `hddBusy()`/
`floppyMotorOn()` consume-and-clear that latch, the same "pulse-stretched
to since last read" convention `altair8800/web/wasm_machine.cpp`'s
`int_seen_`/`busActivityCounts()` already use for their own once-a-frame-
polled indicators. The LEDs themselves were also made larger and
labelled ("motor" / "activity") rather than bare, easy-to-miss dots.

**Verified**: 165/165 native tests pass (10 new: `EgaRenderTest`'s mode-
detect/CGA-decode/dispatch cases). Then, against the real, live site (not
just the native harness): mounted the Oregon Trail floppy through the
actual browser file picker, typed `A:` and `OREGON CGA` (a headless-
Chromium check needed the real Shift+Semicolon key sequence for `:` --
Playwright's high-level `keyboard.type()` doesn't reliably send one, a
testing-tool quirk, not an app bug: a real keyboard reports Shift and the
letter as two independent, properly-sequenced events, which is exactly
what this page's per-key-code architecture already expects and handles
correctly), and got the genuine MECC splash screen and Oregon Trail main
menu rendering correctly at 320x200, canvas auto-resized, floppy motor
LED confirmed lit during the actual load.

## 16. Native 16-color EGA graphics rendering, and a real BIOS/hardware mismatch

Follow-up to §15, from continued real-world use: plain `OREGON` (no `CGA`
argument) still rendered a plain black screen even after §15's fix.
Traced with the same evidence-based method -- boot the real game, dump the
actual registers, don't guess -- and found something more interesting than
a second missing mode: `GR05` Shift Register = 2, and `SEQ[4]` bit 3
(Chain-Four) set. That's VGA's 256-color mode 13h, a mode that **doesn't
exist on real 1984 EGA silicon at all**. It happens because this
machine's freely-licensed BIOS substitute (`VGABIOS-lgpl-latest.bin`) is a
full VGA BIOS -- already flagged as a real tension in §6 ("this machine's
own EGA device is what actually enforces the real EGA ceiling regardless
of what modes this BIOS thinks it can offer") -- and Oregon Trail's own
auto-detect logic, seeing genuinely VGA-class capability reported, picks
VGA's best mode instead of EGA's. A real EGA card, wired to this same
substitute BIOS, would have exactly the same problem: the BIOS would
offer a mode the card physically cannot display. This is a firmware/
hardware mismatch this project already anticipated and accepted, not a
new bug -- `OREGON CGA` (explicitly forcing the CGA-compatible path) is
the period-correct thing to do here, precisely per the game's own
README, and continues to work exactly as §15 verified.

**Separately, and worth building regardless**: native 16-color EGA
graphics (`GR05` Shift Register = 0 -- real modes 0x0D/0x0E/0x10,
320x200/640x200/640x350) was still unimplemented, a real gap distinct
from the mode-13h mismatch above. Fixed properly, grounded in verified
real data rather than a remembered spec: this machine's own BIOS was
invoked directly (`cpu.ax = 0x0010; cpu.interrupt(0x10);` -- a real INT
10h injection, pushing flags/cs/ip and jumping through the real-mode IVT
exactly like the instruction would) to set genuine mode 0x10, then the
actual CRTC registers it programmed were read back: Horizontal Display
End = 79 -> (79+1)*8 = 640, Vertical Display End = 349 (0x5D plus the
Overflow register's bit 1) -> 349+1 = 350 -- exactly real mode 0x10's
resolution, confirming the general formula (not a hardcoded mode table)
before writing a line of the renderer. `ega.h` gained
`crtc_horizontal_display_end()`/`crtc_vertical_display_end()` accessors
(same "host/front-end convenience, reads straight from the existing
register arrays" pattern as every prior addition); `ega_render.h`/`.cpp`
gained `RenderEgaNative16Screen()` (odd/even chaining disabled in this
mode -- linear addressing across all 4 planes; the CRT controller reads
the same plane offset from all 4 planes every cycle, since planar VRAM
always answers on all 4 planes at once, and combines each pixel's 4 bits
-- plane 0 = LSB through plane 3 = MSB of the color index, the standard
EGA/VGA convention -- into the live Attribute Controller palette, same as
every other mode) and a new `ScreenMode::kEgaGraphics16`, distinct from
`kUnsupportedGraphics` (which now specifically means Shift Register = 2,
correctly documented as VGA-only rather than "not implemented yet").

**Verified**: a second native probe drew an actual rectangle through the
real Write Mode 2 path (the standard technique real EGA graphics software
uses to plot a solid color -- CPU byte's bits select per-plane, not a raw
`vram` poke) after invoking real mode 0x10, and the resulting frame showed
a correctly-positioned, correctly-colored rectangle at exactly the
expected pixel coordinates and palette color, at the correct verified
640x350 resolution. 10 new/updated `EgaRenderTest` cases (167/167 total).
The WASM front end needed no changes at all beyond rebuilding -- 
`wasm_machine.cpp`'s `renderFrame`/`renderWidth`/`renderHeight` already
dispatch through the same shared `RenderScreen()`, and `app.js`'s canvas-
resize logic was already generic (proven by §15's CGA-mode resize)
rather than hardcoded to particular dimensions.

**Also fixed while investigating**: the local LAN preview server
(`python3 -m http.server`, used by `make -C retroweb preview`/
`preview-bg`) sent no cache headers at all, unlike every machine's own
`web/devserve.py` -- a real, separate bug that made a stale wasm module/
app.js indistinguishable from an actual regression after re-staging.
Replaced with `retroweb/serve_nocache.py` (same `Cache-Control: no-store`
handler devserve.py already used), wired into all three `preview`/
`preview-bg`/`preview-install` code paths.

## 17. Site-wide theme sync, a bigger screen, the realistic front panel, and power/reset

Follow-up requests: match the page theme to altair8800/cg-oac-6502's own,
maximize the screen, and build a real-looking front panel (drive bays,
HDD/power LEDs) with power and reset switches, inspired by altair8800's
front panel -- plus, explicitly, delegate well-scoped mechanical work to
a cheaper model where it fits.

**Delegation**: a Haiku subagent synced every shared theme token (colors,
fonts, shadows for all four themes) to altair8800/web/index.html's exact
values, scoped strictly to the four `:root`/`:root[data-theme=...]`
custom-property blocks with an explicit "touch nothing else" instruction
-- a genuinely mechanical, fully-specified, low-integration-risk task
(CSS variable *values* can't rename an element ID or break a JS
selector). The front panel's HTML/CSS and the power/reset behavior were
done directly instead: real hardware semantics (what powering off/on and
resetting actually do to running state) and deep integration with
existing JS hooks (`.at-bay[data-drive]`, `[data-role=...]`, `#hddLed`)
are exactly the kind of work this project's own CLAUDE.md guidance says
to keep rather than delegate -- getting it wrong is expensive to catch
later, and a fresh agent would have to re-derive context already in hand.

**A real bug, initially misdiagnosed**: `.page`'s `max-width: var(--page-max)`
computed as `none`, confirmed three ways (`getComputedStyle().maxWidth`,
`getBoundingClientRect().width`, a screenshot) -- but the actual page
looked *fully themed* in every other respect in that same screenshot
(correct panel/button/titlebar colors), so the fix applied at the time
was scoped to just this one property: swap `var(--page-max)` for a
literal `920px`. That was a real fix for that one symptom, but it wasn't
the disease, and the "every other respect looked themed" read turned out
to be wrong too -- a user comparing this page side by side with
altair8800's caught that the *entire* Windows 95 and Mid-1990s Web themes
were rendering essentially unstyled (plain white page, no titlebar
gradient, no panel borders), which a quick visual scan had missed by
focusing on layout/function rather than actually comparing colors against
the reference page. The real cause: this file's own theme-block comment
header closed with an HTML comment terminator (`-->`) instead of a CSS
one (`*/`) --

```
/* ---- page themes: ...
   ... front panels. -->        <- should be */
```

-- so the CSS comment was **never closed**. Everything from that point
was silently consumed as "still inside the comment" until the parser
hit the next literal `*/` anywhere later in the file (which happened to
be one line inside the *fourth* theme block, `moderndark`'s own trailing
comment) -- meaning the `:root`, `web94`, and light-`modern` blocks (three
of four theme blocks, the whole non-dark-mode token set) were being
dropped by the parser on every load. Confirmed precisely by inspecting
`document.styleSheets[0].cssRules` directly: 93 rules instead of the
expected ~96, with rule 0 being the `moderndark` block instead of the
base `:root`. Fixed by closing the comment correctly; `--page-max` was
reverted back to the token-driven form (`.page { max-width:
var(--page-max); }`, `:root[data-theme="modern"] { --page-max:
min(1600px, 95vw); }`) now that the actual disease, not just that one
symptom, is fixed. Take-away for next time: when a computed style comes
back wrong, check whether the *whole* stylesheet parsed as expected
(`document.styleSheets[...].cssRules.length` and `cssRules[0]`) before
assuming the one property under investigation is the whole story --
and a "looks themed" visual skim is not the same as actually comparing
colors/chrome against the reference page pixel-for-pixel.

**Screen size**: base CSS width raised from 640px to 860px (matching the
spirit of altair8800's own 792px-wide CRT) -- still `max-width:100%` and
still whatever the actual current-mode resolution is (640x350 text,
320x200 CGA graphics, etc.) upscaled with `image-rendering:pixelated`,
not a change to any real resolution.

**Front panel**: one `.at-case` fascia (beige AT-plastic gradient, real
inset/outset shadows for a moulded-plastic look) replacing the separate
"Floppy drives"/"Hard disk" panels, holding both floppy bays (each with
a slot + door-lip graphic and a diskette that visibly "sticks out" of
the slot when loaded -- the same idea as altair8800's own `.dcdd-slot`,
adapted from an 8-inch to a 5.25-inch drive's proportions) and the HDD
activity LED together, matching how these all live on one real physical
bezel. Every existing `data-role`/`data-drive` hook app.js already used
was preserved exactly; only the CSS classes and the addition of a
`.loaded` state (driven by a small `setBayLoaded()`/`setBayEmpty()` pair
in app.js, replacing several call sites that used to poke the DOM
directly) changed.

**Power switch (default off) and reset**: a real physical-looking rocker
switch (a visually-hidden real `<input type=checkbox">` under a styled
`<span>`, the standard accessible-custom-control pattern -- real user
clicks the label and it works via native label-to-input association;
Playwright's `.check()` can't target the visually-hidden input directly
and needs to click the label instead, a test-tooling detail worth noting
for Phase 8, not a page bug) and a round reset button. Modeled on real
AT hardware semantics, not just a UI nicety: powering off discards the
whole `Machine` instance (RAM is genuinely gone the instant power is
cut, exactly like unplugging it) and blanks the canvas; powering back on
builds a fresh `Machine`, reloads the already-fetched firmware bytes
(fetched once, up front, regardless of power state, since that's the web
delivery mechanism, not anything physical), and remounts whatever
floppy images were still "in the drive" (tracked separately in JS,
surviving the discarded `Machine`) -- a real diskette stays seated
whether or not the computer itself has power. A real reset button is
different hardware: it pulses the CPU's RESET line without touching
power, so RAM and any seated diskette are untouched -- exactly
`Machine::reset()`'s already-existing real semantics, needing no new
C++ at all. Floppy insert/eject now works whether the machine is
powered on or off, matching a real mechanical drive; inserting while
off and then powering on was verified to correctly remount it.

**Verified**: 167/167 native tests (no C++ touched this pass). Headless-
Chromium, against the live site: power switch unchecked by default;
reset clickable while powered on; modern theme's page width measured
920px at 2000px viewport before the fix and correctly ~1600-1330px
(the `min(1600px, 95vw)` formula) after it, while Windows 95/web94 stay
at the fixed 920px cap at the same viewport width; full boot to a live
`C:\>` with the new front panel, power LED lit, screenshotted; powered-
off state screenshotted (blank screen, dark LEDs, disabled reset);
insert-while-off-then-power-on remount confirmed directly.

## 18. Front panel, round two: closer to the genuine 5170 bezel

Follow-up pass after seeing a real 5170 reference photo next to the §17
front panel: the case fascia was right in spirit but wrong in several
concrete details against the genuine machine, corrected here one for
one.

**No front-panel reset button.** The genuine IBM 5170 has no
dedicated reset control at all -- the round reset button in §17's
panel was a later clone/compatible-era convention, not period-accurate
to this machine, so it's removed outright rather than relabelled.
`app.js`'s `resetButton` variable, its enable/disable toggles in
`powerOn()`/`powerOff()`, and its click handler are all deleted;
`Machine::reset()` itself is untouched C++ (nothing else in this
codebase calls it now, but the method stays -- it's a real, correct
piece of machine semantics, just currently unreachable from the front
end).

**No boxed "hard disk" panel.** The real machine's HDD activity light
is a bare LED on the bezel, not a labelled sub-panel with its own
caption text. §17's separate `.at-hdd` box (border, "HDD activity"
caption, drive-capacity/OS text) is gone; `#hddLed` is now just another
dot in the same `.at-indicators` row as the power LED, unlabelled like
its real counterpart. Same treatment for the floppy activity LEDs and
the power LED: no more `.led-caption` spans anywhere on the panel ("bus
drive motor", "power", etc.) -- the switch keeps its own "POWER" text
because that's a control a user operates, not a light being explained.

**Stacked bays, not side-by-side.** The reference photo shows the two
5.25" half-height floppy bays stacked vertically, one above the other;
§17 had them side-by-side (closer to the Altair 88-DCDD cabinet's own
layout, which was the wrong reference to imitate here). `.at-drives`
is now `flex-direction: column`, and each `.at-bay` is its own full-
width horizontal strip (drive letter, slot, latch, activity LED on one
line; filename label + Insert/Eject on the line below) rather than a
grid cell in a shared row.

**Both drives modeled as 5.25", deliberately.** The request asked for
the drives to "look like their real counterparts (5.25 vs 3 inch
disks)" -- worth being explicit that this build does *not* give drive
B a 3.5" face. Per this repo's own §7/§11 scope decisions (and
`fdc765.h`'s existing geometry, unchanged this pass), both emulated
floppy drives are real 5.25" formats: Drive A is 1.2MB (80 cyl/2
head/15 sec, 360 RPM), Drive B is 360KB (40 cyl/2 head/9 sec, 300 RPM).
A real 5170 of this vintage shipped this way -- 3.5" drives didn't
reach the PC platform until the AT-compatible clones and the PS/2 era.
Giving drive B a 3.5" bezel here would misrepresent the hardware this
emulator actually models, so both bays get the same slot/latch
treatment, distinguished only by their capacity label (now suffixed
`, 5.25″` on both, via `driveDefaultLabel()` in app.js) -- a case of
this repo's realism rule cutting against the surface reading of the
request rather than for it.

**Vents and latches, dropped diskette-peek effect.** Added a decorative
`.at-vents` slat block (the left-hand brand-plate/vents column mirrors
the reference photo's proportions) and a small `.at-latch` bar per bay
representing the real lever-style latch mechanism of this drive era.
The previous "diskette visibly peeking out of the slot" effect
(`.at-bay.loaded .at-slot::before { content: attr(data-label) }`) was
dropped in this layout -- the new compact horizontal bay strip has no
good place to hang a protruding label -- in favor of just a background
shift on `.at-slot` when `.loaded`; the actual filename is still shown
in the `.drive-label` text beneath each bay, so no information was
lost, only the cosmetic peeking-paper visual.

**Screen focus outline removed.** `#screen:focus` was `outline: 2px
solid var(--link)`; changed to `outline: none` per explicit request.
The screen has its own visible black bezel/background already, so
losing the browser's default focus ring doesn't leave keyboard users
without any indication of the interactive region -- the "Click the
screen, then type" status line above already carries that instruction.

**Power switch position kept on the front panel anyway.** The user
confirmed the genuine 5170's power switch lived on the side/rear of
the case, not the front bezel, and confirmed this is fine as-is: the
switch stays on the on-screen front panel as a labelled web-UI
concession (there's no "side of the browser window" to put it on),
consistent with this repo's own opt-in-override rules -- it was never
presented as a claim about where the real switch sat.

**Verified**: 167/167 native tests (no CPU/chipset/device C++ touched
this pass -- HTML/CSS/app.js only). Headless Chromium against the live
site: `#screen:focus` computed `outlineStyle === 'none'`;
`document.getElementById('resetButton')` is `null`; full power-on boot
to a live `C:\>` prompt screenshotted with the redesigned panel visible
-- brand plate, vents, bare green power LED + dark HDD LED, rocker
switch, two stacked bays each showing their `empty (1.2MB, 5.25″)` /
`empty (360KB, 5.25″)` labels, slot, latch, and Insert/Eject controls,
matching the reference photo's layout.

## 19. A real missing-background bug, plus display/panel sizing follow-ups

Another real, concrete bug surfaced by a side-by-side look at the
Mid-1990s Web theme: `:root[data-theme="web94"] body { background: ...
url(...) repeat; }` -- the tiled "blue marble" desktop texture every
other machine's `web94` theme uses -- was never actually copied into
this file. The §17 Haiku-subagent sync was scoped strictly to the
`:root`/`:root[data-theme=...]` *token* blocks, and this rule lives
outside them (it's a `body` rule keyed off the same selector, not a
custom-property declaration), so it was correctly out of scope for that
sync and nobody added it by hand afterward -- the page still "looked
themed" because the pagebar/panel chrome all come from tokens that did
sync, so only the desktop background itself was silently plain grey.
Fixed by copying the exact same rule (and its identical base64 JPEG
asset) from `altair8800/web/index.html` verbatim.

Three more follow-up requests, all straightforward:

- **Removed** the `<p class="muted">Click the screen, then type --
  keystrokes go straight to the keyboard controller.</p>` line entirely
  -- the `#bootStatus` line above it already carries the operational
  state, and the instruction was judged redundant clutter rather than
  something to reword.
- **Display now expands to fill the modern theme's width.** Previously
  `#screen` was a flat 860px in every theme, stranding it small inside
  modern's much wider `--page-max: min(1600px, 95vw)` page. Added a
  modern-only override (`.monitor` and `#screen` both `width: 100%`,
  `#screen` capped at `max-width: 1400px`) -- `height: auto` plus the
  `aspect-ratio` app.js already keeps in sync with the current video
  mode does the rest, so this is purely a bigger upscale of whatever
  the real resolution is, never a change to any actual resolution.
  Other themes keep the original fixed 860px CRT size.
- **Front panel now has a fixed width** (`max-width: 640px; margin: 0
  auto` on `.at-case`) instead of stretching to match `.page`'s width --
  a real case fascia doesn't get physically wider just because the
  browser window did, and the previous stretch was making the drive-bay
  slot bars absurdly long and thin in the wide modern layout.
- **Drive-to-grille ratio corrected**: the vents column (`.at-left`)
  narrowed from a 140px minimum to a fixed 96px, and the floppy slot
  itself grew from a 16px sliver to a 30px opening (with matching
  padding/latch bumps) -- together this makes the two drive bays read
  as substantial physical units and the ventilation grille a
  proportionally narrow strip beside them, closer to the real case's
  actual proportions instead of two visually-equal-weight blocks. The
  brand-plate text was shortened to just "IBM" / "5170-339" (dropping
  "Personal Computer AT · ") since the full string no longer fit the
  narrower column without wrapping into several lines.

**Verified**: 167/167 native tests (HTML/CSS only, no C++ touched).
Headless Chromium: `web94` theme's `body` computed
`background-image` is non-empty (tiled texture confirmed present); the
removed paragraph's text no longer appears anywhere in the page;
at a 1800px-wide modern-theme viewport, `#screen` measured 1400×765.6
(ratio 1.829, matching 640/350 exactly) while `.at-case` held at a
fixed 640px regardless of the 1600px-wide `.page` around it;
screenshotted both the Mid-1990s Web theme (tiled background visible
around the window) and the wide modern theme (full-width display,
fixed-width front panel) to confirm visually.

## 20. Front panel round three: real case proportions, and per-theme width

A real reference photo made two more things obvious: the grille+badge
cluster on a genuine 5170 is the *dominant* width of the bezel (roughly
3/4 of it), with the two stacked floppy bays occupying a comparatively
narrow column at the right -- not a roughly-even split, which is what
§18/§19's flex layout (`.at-left` a fixed 96px, `.at-drives` taking
essentially everything else) actually produced. And the front panel's
own width policy needed to match the CRT's: stretch with `.page` in the
Windows 95 / Mid-1990s Web / dark-default themes (all fixed around
920px anyway, so nothing runs away), but stay capped in modern's much
wider layout.

**Ratio fix**: `.at-left` and `.at-drives` are now flex ratios (`flex:
3 1 0%` / `flex: 1 1 0%`) instead of one fixed-width column and one
that eats the remainder -- measured at a 1400px viewport in the
Mid-1990s Web theme, that lands at a 71/29 split, close to the
photo's roughly-3/4 grille width. Restored the full brand-plate text
("IBM Personal Computer AT · 5170-339") now that `.at-left` has real
room again -- §18's shortened "IBM 5170-339" was only ever a fix for
the old fixed-96px column, not a considered final wording.

**Width fix**: `.at-case` dropped its own `max-width` and now takes
`width: 100%` (stretching with `.panel`/`.page` like every other
control on the page, in every non-modern theme); a
`:root[data-theme="modern"] .at-case { max-width: 640px; margin: 0
auto }` override keeps it from following modern's much wider
`--page-max` the way the CRT deliberately does.

**Narrow-column fallout**: the drive-bay column is now genuinely
narrow (~190-280px depending on theme/viewport), too tight to fit the
capacity label and both Insert/Eject buttons on one line without
truncating the label (`empty (1...` was the actual regression caught
mid-fix). Changed `.at-bay-ctl` to `flex-wrap: wrap` with
`.drive-label { flex: 1 1 100% }` so the label always gets its own full
line and the buttons wrap to the line below, rather than fitting a
narrower ellipsis-truncated label net to them.

**Verified**: 167/167 native tests (HTML/CSS only). Headless Chromium
at a 1400px viewport in the Mid-1990s Web theme: `.at-case` measured
834px (matching `.page`'s fixed width), split 589.5/196.5 between
`.at-left`/`.at-drives` (a 0.71 ratio); at a 1800px viewport in the
modern theme, `.at-case` held at exactly 640px regardless. Both drive
labels (`empty (1.2MB, 5.25″)` / `empty (360KB, 5.25″)`) render in
full, unterminated, with Insert/Eject wrapped to their own line below;
screenshotted in both themes to confirm the grille-dominant proportions
read correctly.

## 21. Front panel round four: lights/switch above the grille, wider drives

Explicit follow-up: "close enough realism but function over form" --
i.e. stop chasing the reference photo's exact proportions once the
controls start feeling cramped, and prioritize usability. Two changes:

- **Indicator lights and the power switch moved above the grille**,
  matching the real 5170's own layout (the keylock/LED cluster sits
  above the ventilation slats, not below them). Wrapped
  `.at-indicators` and the `.at-switch-group` in a new `.at-toprow`
  flex row, reordered in the markup to come right after the brand
  plate and before `.at-vents`.
- **Drives widened**: §20's `.at-left`/`.at-drives` flex ratio (3:1,
  ~75/25) was true to the photo but left the drive-bay controls
  uncomfortably tight. Eased to 2:1 (measured 63/37 at a 1400px
  viewport), and bumped `.at-drives`'s `min-width` from 190px to
  230px -- still grille-dominant like the real machine, just not
  starved for it. `.at-vents`'s `min-height` trimmed from 50px to 40px
  since the toprow now takes some of the vertical space it used to
  have alone.

**Verified**: 167/167 native tests (HTML/CSS only). Headless Chromium
at a 1400px viewport in the Mid-1990s Web theme: `.at-left`/`.at-drives`
measured 524px/262px (0.63 ratio, up from 0.71); modern theme still
held `.at-case` at exactly 640px at a 1800px viewport. Screenshotted
both to confirm the LEDs/switch now sit above the grille and the drive
bays read less cramped.

## 22. Front panel round five: square boxed lights, inset grille border

Another reference-photo detail pass, this time on the LED cluster and
the grille's own framing:

- **Each indicator light now sits in its own small recessed black box**
  (`.led-box`, 15x15px, dark background + inset shadow) instead of
  floating bare on the case plastic -- the real panel's power/HDD
  lights sit in individually-boxed instrument-style cutouts, not plain
  dots. `#powerLed`/`#hddLed` are unchanged elements (still toggled by
  id in app.js, oblivious to the new wrapper), just now wrapped in a
  `<span class="led-box">` each.
- **The lights themselves are square, not round** -- `.at-indicators
  .drive-led { border-radius: 1px }` overrides the shared circular
  `.drive-led` shape (which stays round everywhere else it's used, i.e.
  the floppy-bay activity LEDs -- those aren't the lights the reference
  photo shows, so left as-is).
- **The grille now has a visibly wider left and bottom border** than
  its top/right (`margin: 0 4px 14px 16px` on `.at-vents`), matching
  the real bezel's own plain-plastic framing around the vent field --
  previously the vents ran flush to the edges of `.at-left` on every
  side.

**Verified**: 167/167 native tests (HTML/CSS only). Rebuilt and
screenshotted the Mid-1990s Web theme at a 1400px viewport; a cropped
close-up of the panel confirms two square green/dark boxed lights next
to the power switch, and a clearly inset grille with extra left/bottom
margin, matching the reference photo's proportions.
