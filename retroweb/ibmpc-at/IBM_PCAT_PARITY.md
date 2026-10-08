# IBM PC/AT 5170-339 -- parity backlog

The open gaps between this machine and a real 1986 8 MHz 5170-339 with an
IBM EGA, plus the test gaps against the repo rules in `CLAUDE.md`. Most of
the chip files started as the same code `pc486` grew from, so a lot of
this list is `PC486_PARITY.md` work that never came back. Where a 486 fix
ports, the item cites its `PC486_REVIEW.md` section. Checked against the
code as of §50 (the 8259A and 8237A). When an item is fixed, write it up in
`IBM_PCAT_REVIEW.md` as usual (fact, why it matters, what it fixed,
source) and delete it here. Done so far: the real-mode CPU, C1-C9
(`IBM_PCAT_REVIEW.md` §44), the reset vector and `F1h`, C11-C12
(§45), bus timing, T1-T5 and T8 with P5 and E11 (§46), and EGA
contention plus the TOPBENCH calibration, T6-T7 with P1-P3 (§47), and
the EGA BIOS stand-in plus E1-E10 and E12-E14 (§48), and EGA
addressing, latches and the monochrome ports, E15-E17 (§49), the
full 8259A and 8237A with P4, P6 and P9 (§50), and storage, S1-S4
(§51) and S5, S6 and S8's rotation (§52), and the keyboard and
RTC, K1-K5 and R1 (§53).

Rough parity today: **~85%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| CPU (real mode) | 98% | the IDT limit isn't checked; no STOREALL |
| CPU (protected mode) | 0% | not implemented at all |
| Timing | 96% | fetch-bound code runs 4-7% fast against TOPBENCH |
| EGA | 92% | memory-cycle length unsourced (VidMem -9% on the Enhanced Display) |
| Chipset (PIT, PIC, DMA, memory) | 80% | no extended memory, no serial or parallel port |
| Storage (WD1003, floppy) | 95% | floppy data rate not enforced, hard disk seeks are instant |
| Keyboard, RTC | 95% | no keylock control, BAT arrives without its self-test delay |
| Speaker | 95% | not checked against a recording of a real 5170 |

Every fix lands with its native test and, where it's visible from the
page, a Playwright test, in the same commit.

# TODO

## 1. CPU

- **C10. No protected mode.** §1 scoped it out because DOS never leaves
  real mode. But the AT's own BIOS uses it (INT 15h AH=87h block move and
  AH=89h), and so do HIMEM.SYS, VDISK, RAMDRIVE, Windows 3.0 standard
  mode, OS/2 1.x and Xenix 286. FreeDOS's installer, now that it sees a
  286, makes `FDXMS286.SYS` the default boot choice (§44.8). The
  real-mode side is done: SMSW, LMSW, CLTS and the descriptor-table loads
  and stores work (§44.4). Still needed: setting PE (LMSW reports and
  drops it today), LLDT/LTR and the rest of the 0F 00 group, descriptor
  caches, the 286 descriptor and gate formats, privilege and limit
  checks, task switches, the real-mode IDT limit check, and the
  triple-fault shutdown that the board turns into a CPU reset (port
  §40.6). The no-way-back-to-real-mode rule and the 8042 reset trick
  (already here) belong with it, and so does undocumented LOADALL
  (`0F 05`, a reported no-op today), which HIMEM 2.x and RAMDRIVE used on
  286s. Same scale as the 486's T1: its own design pass first. Lands with
  P8. Source: Intel iAPX 286 PRM.

## 2. Timing

- **T9. Fetch-bound code runs fast.** Against the real 5170-339 in
  TOPBENCH's database, MemEA is 7% fast, Opcodes 5%, 3DGames 4% (§47.4).
  MemEA sits on the bus limit the data sheet and §46.1 allow, so the real
  286 leaves its bus idle about 0.85 clocks per instruction somewhere the
  published timings don't describe. Needs a logic-analyzer trace of a
  real 286 bus unit or a cycle-level 286 model to source, not a fitted
  constant.

## 3. EGA

- **E18. Write mode 3 on a real EGA.** GR05 bits 0-1 = 3 is VGA's write
  mode 3; the EGA TR lists it as not valid and doesn't say what the card
  does. `Ega` runs it as VGA does. MAME's EGA does the same, 86Box's
  writes nothing, and neither cites hardware. Needs a test on a real IBM
  EGA. Until then there's nothing to port.
- **E19. No 9x14 alternate glyphs.** In mode 7, IBM's ROM patches the 8x14
  font with 9-dot versions of about a dozen characters. The only open set
  found (DOSBox's) is GPL and looks copied from IBM's ROM, so it can't go
  in the stand-in. AH=11h AL=30h BH=5 returns an empty list. Drawing new
  glyphs wouldn't make the screen any closer to IBM's, so this stays open
  until a clean public-domain set turns up.
- **E20. EGA memory-cycle length isn't sourced.** §47.3 derives 32 dots
  per five memory cycles. With the Enhanced Display, TOPBENCH's VidMem
  reads 1335 against the real 5170-339's 1470 (-9%); with a 200-line
  colour display it reads 1514 (+3%). The real run didn't record its
  monitor (§47.4). Needs a VidMem reading from a 5170 with an EGA and a
  5154, or the cycle timing from the EGA's sequencer documentation.

## 4. Chipset

- **P7. No serial or parallel port.** The 5170-339 came with IBM's
  Serial/Parallel Adapter: COM1 at 3F8h (IRQ4, an NS16450) and LPT1 at
  378h (IRQ7). Opens a serial mouse, which is how an AT got one. Needs a
  front-end story for the other end. Confirm the 339's standard fit in
  the 5170 Guide to Operations. Source: IBM PC/AT Technical Reference,
  Serial/Parallel Adapter; NS16450 data sheet.
- **P8. No extended memory.** `Chipset::mem` is exactly 1MB, so the A20
  gate has nothing above it, the HMA reads FFh, and CMOS 17h/18h and
  30h/31h say zero. The 339 had 512KB on the system board, and owners
  routinely added extended memory on an adapter. Model 640KB base (a
  labelled choice over the bare 512KB) plus an extended-memory size and
  24-bit addressing. The ROM alias at the top of 16MB is already there
  (§45.1). Source: IBM
  PC/AT Technical Reference, system memory map. Lands with C10:
  FreeDOS's 286 menu loads FDXMS286, which today declines for lack of
  extended memory and would otherwise need LOADALL or INT 15h AH=87h.
- **P10. No game port.** The IBM Game Control Adapter at 201h was an
  option, not standard. Low priority. Port P7 from the 486 if it lands
  there first.
- **P11. 8237 features nothing here starts.** A software request (request
  register) on a block-mode channel should run the transfer, including
  memory-to-memory on channels 0 and 1 through the temporary register.
  Command bits 3, 5, 6 and 7 (compressed timing, extended write, DREQ and
  DACK polarity) and rotating priority are stored and ignored. Low
  priority: no AT device or BIOS path uses them. Source: Intel 8237A-5
  data sheet (`IBM_PCAT_REVIEW.md` §50.3).

## 5. Storage

- **S7. The floppy data rate isn't enforced.** 3F7h is latched (§51.4),
  but a mismatch with the media still reads, because the stand-in BIOS
  never writes the register. On a real AT, a 360KB disk reads only at
  300 kbps in A: and 250 kbps in B:. Needs a BIOS that sets the rate (a
  patch to the Bochs BIOS's media sense, or a different stand-in), then
  the check. Source: WD1003-WA2 OEM manual, Floppy Control register.
- **S8. Hard disk seeks are instant.** The platter turns (§52.4), but
  SEEK, RESTORE and implied seeks take no time. The ST4038 gives 11 ms
  track to track, 40 ms average and 85 ms full stroke, all maximums, and a
  real drive is usually quicker. Needs a seek profile, or a measurement
  from a real drive, not a curve fitted to maximums. Source: Seagate
  ST4038 product manual 1.2.

## 6. Test parity

- **X1. Smoke doesn't prove a boot.** `web/tests/smoke.spec.ts` only
  checks real-speed pacing and says it "only needs the machine running".
  `CLAUDE.md` wants a boot plus one real interaction. Add a case: boot to
  `C:\>` and echo a typed key. Same as the 486's X1.
- **X2. Thin native suites.** machine 11, fdc 14. IRQ routing and the
  cascade are covered in `chipset_test` (§50.5); machine still runs no CPU-level interrupt through both PICs.
- **X3. Controls without a test.** `hddLed` has none, and the F-key and
  extra-key rows are only checked for enabled state and focus, not that
  they deliver a scan code. Every control needs one per `CLAUDE.md`.
- **X4. No coverage figures.** `make coverage` and `make -C web coverage`
  exist but the review has no recorded numbers. Record a baseline before
  this backlog starts, then refresh it.
- **X5. Review corrections.** §1 and §3 still describe protected mode
  as out of scope. Update them when C10 lands.

## Not ported from the 486, on purpose

VBE, the DAC and VGA's 400-line text, the cache model, FPU tags and
SoftFloat (no 80287 fitted; the ESC opcodes doing nothing is right), the
DX signature, #AC and debug registers (386 and later), the 15-byte limit
(the 286's is 10, §44.5), SB16/OPL3/CD-DA, and the PS/2 mouse (the AT has no
aux port; a serial mouse via P7 is the period way).

Already in line with the rules, for reference: the `?test=1&fast=1`
multiplier is gated in `app.js` and the smoke file keeps a real-speed
check; deploy CI runs `ibmpcat-test` / `ibmpcat-web-test` smoke only, and
the full suites run nightly.
