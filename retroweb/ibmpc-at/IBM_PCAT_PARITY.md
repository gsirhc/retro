# IBM PC/AT 5170-339 -- parity backlog

The open gaps between this machine and a real 1986 8 MHz 5170-339 with an
IBM EGA, plus the test gaps against the repo rules in `CLAUDE.md`. Most of
the chip files started as the same code `pc486` grew from, so a lot of
this list is `PC486_PARITY.md` work that never came back. Where a 486 fix
ports, the item cites its `PC486_REVIEW.md` section. Checked against the
code as of commit `7534c4c`. When an item is fixed, write it up in
`IBM_PCAT_REVIEW.md` as usual (fact, why it matters, what it fixed,
source) and delete it here. Done so far: the real-mode CPU, C1-C9
(`IBM_PCAT_REVIEW.md` §44), and the reset vector and `F1h`, C11-C12
(§45).

Rough parity today: **~71%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| CPU (real mode) | 98% | the IDT limit isn't checked; no STOREALL |
| CPU (protected mode) | 0% | not implemented at all |
| Timing | 70% | no memory or I/O wait states, no refresh steal |
| EGA | 60% | graphics ignore the start address, the BIOS says VGA |
| Chipset (PIT, PIC, DMA, memory) | 60% | PIT square waves only, no extended memory |
| Storage (WD1003, floppy) | 75% | thin WD1003 command set, no FDC data rate |
| Keyboard, RTC | 60% | RTC doesn't tick, no typematic, lost bytes |
| Speaker | 90% | rides on the PIT's mode model |

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

- **T1. No memory wait state.** The per-opcode costs (§40) are Intel's
  zero-wait-state numbers, and `cpu80286.h` notes the 5170-339's wait
  state isn't modelled. The 8 MHz AT runs system-board memory at one wait
  state, so each bus cycle is 3 clocks, not 2. Charge it per memory word
  transferred, code fetch included. Source: IBM PC/AT Technical
  Reference, "System Timing".
- **T2. No I/O or 8-bit card wait states.** The AT bus stretches 8-bit
  cycles (the EGA, the FDC, the 8042, every 8-bit adapter) well beyond a
  16-bit memory cycle, and a 16-bit access to an 8-bit card is split into
  two. The EGA is an 8-bit card, so word writes to A0000h cost double.
  Port the approach of `PC486_REVIEW.md` §48.1 with the AT's own numbers.
  Same source.
- **T3. Refresh doesn't steal the bus.** DMA channel 0 refresh, clocked
  by PIT channel 1 every ~15 us, takes the bus for a few clocks each
  time, about 5-7% of the CPU on a real AT. Benchmarks of the period
  measure it. Ties to P5. Same source.
- **T5. IRQ delivery isn't charged.** `Machine::run_cycles()` ignores the
  cycles `Cpu::interrupt()` returns for a hardware interrupt, so the
  INTA cycles and the vector fetch cost no wall-clock time. That figure
  (45) also disagrees with INT n's 23. Settle the number from the iAPX 286
  timing appendix, then charge it.
- **T4. EGA retrace timing is fixed.** `Ega::tick` uses a flat 60 Hz with
  an 8% retrace window, not the CRTC's programmed totals, and has no
  horizontal timing at all. Port `recompute_timing_()` from the 486
  (§8.6) with the EGA's 14.318 / 16.257 MHz dot clocks from Misc Output
  bits 2-3. Source: IBM Enhanced Graphics Adapter Technical Reference.

## 3. EGA

- **E1. The video BIOS is a VGA BIOS.** `VGABIOS-lgpl-latest.bin` answers
  INT 10h AH=1Ah as a VGA, so programs that probe pick VGA paths: they
  load the DAC at 3C8h/3C9h (not decoded here), ask for mode 13h or 12h
  (rendered black), or size the screen for 480 lines. The IBM EGA BIOS
  is IBM's copyright, so the answer is a stand-in that reports an EGA
  (AH=1Ah unsupported, AH=12h BL=10h returning EGA and its memory size),
  built from the LGPL source with only EGA modes, and labelled as a
  stand-in in the README and boot banner. Source: IBM EGA Technical
  Reference, BIOS interface.
- **E2. Graphics ignore the start address.** `RenderEgaNative16Screen`
  draws from offset 0 every frame. EGA games page-flip and scroll by
  writing CRTC 0Ch/0Dh, so double-buffered games show the wrong page or
  never move. The CGA-compatible renderer has the same problem (port
  §43.2). Source: IBM EGA Technical Reference, CRTC.
- **E3. No pel panning.** AR13 and CRTC 08h (preset row scan) are
  ignored. Commander Keen's smooth scroll is the classic EGA use. Port
  §42.2.
- **E4. No line compare.** CRTC 18h plus Overflow bit 4 for split
  screens. Port §42 (the EGA has 9 bits, not the VGA's 10). Same source.
- **E5. Text is always 25 rows.** `ega_render.cpp` hard-codes `rows =
  25`, so the EGA's 43-line mode (8x8 font in 350 lines, `MODE CON
  LINES=43`) draws the top 25. Derive rows from Vertical Display End and
  Max Scan Line. Port §42.3. EGA text stays 8 dots wide, unlike the 486.
- **E6. Attribute bit 7 is dropped.** AR10 bit 3 picks blink or bright
  backgrounds. Blink runs off the vertical sync count. Port §42.3 and
  §43.1. Same source.
- **E7. Character map select (SR03) is ignored.** Two fonts, and
  512-character mode through attribute bit 3, with 128KB or more on the
  card. Same source.
- **E8. Registers read back like a VGA.** On a real EGA almost every
  register is write-only: only CRTC 0Ch-0Fh (start address, cursor) and
  10h-11h (light pen) read back, and 3CCh doesn't exist. Programs tell an
  EGA from a VGA exactly this way. Return open bus for the rest. Same
  source.
- **E9. Input Status 0 reads 00h.** 3C2h bit 4 is the switch sense the
  BIOS reads the card's DIP switches through, and bit 7 is the vertical
  retrace interrupt flag. Same source.
- **E10. No vertical retrace interrupt.** CRTC 11h bits 4-5 enable and
  clear an interrupt on IRQ2 (IRQ9 on the AT's slave PIC). Some period
  games time to it. Same source.
- **E11. Input Status 1 bit 0 never moves.** 3DAh returns only bit 3.
  Bit 0 (display enable, inverted) is what programs poll for horizontal
  timing and snow-free updates. Port the 486's 22fafe9 fix. Needs T4.
- **E12. Mode 06h and CGA interleave.** The 16-colour renderer ignores
  CRTC 17h bit 0 (CGA compatibility addressing), so 640x200 two-colour
  mode, which the EGA runs through the CGA odd/even banks, likely renders
  wrong. Check against the IBM EGA mode table before changing it.
- **E13. Aspect ratio.** `app.js` sets `aspectRatio` to the pixel size, so
  640x350 and 320x200 are drawn too short. The 5154 Enhanced Color
  Display is a 4:3 tube in every mode. Port §42.4 and keep the integer
  pre-scale on each axis.
- **E14. Cursor skew and underline.** CRTC 0Ah/0Bh skew bits and the
  underline location register. Port §43.3.

## 4. Chipset

- **P1. PIT modes 0, 1, 4, 5 are square waves.** Every mode runs as mode
  3. Mode 0 one-shots fire periodically and Landmark-style mode 2 timing
  is off. Port `pit8253.cpp` from `pc486` (§41.1). Source: Intel 8254
  data sheet, "Mode Definitions".
- **P2. Mode 3 doesn't count by two.** Visible on any counter readback.
  Same port.
- **P3. No 8254 read-back command.** `pit8253.cpp` drops control word
  `11xxxxxx`. The AT has an 8254, so it has read-back. Port §41.2.
- **P4. No PIC poll command.** OCW3 with P=1. Port §41.3. Source: Intel
  8259A data sheet, "The Poll Command".
- **P5. Port 61h bit 4 flips every instruction.** `Chipset::tick` toggles
  it on every pass. It should follow PIT channel 1's refresh request,
  about every 15 us with the BIOS's count of 18. Channel 1 isn't
  modelled at all yet. Same item as the 486's P8. Source: IBM PC/AT
  Technical Reference, port 61h.
- **P6. ICW1 doesn't clear the mask.** ICW1 clears IRR and ISR here but
  leaves IMR, where the 8259A clears IMR. Same bug in `pc486`. Source:
  Intel 8259A data sheet, "Initialization Command Words".
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
- **P9. DMA wraps at 1MB, not at the page.** The FDC copy indexes
  `mem[(addr + i) & 0xFFFFF]`, so a transfer that crosses a 64KB boundary
  carries into the next page. The 8237's address counter is 16 bits and
  the page register doesn't increment, so it wraps within the page.
  Source: Intel 8237A data sheet; IBM PC/AT Technical Reference, DMA
  page registers.
- **P10. No game port.** The IBM Game Control Adapter at 201h was an
  option, not standard. Low priority. Port P7 from the 486 if it lands
  there first.

## 5. Storage

- **S1. Thin WD1003 command set.** Only RESTORE, READ, WRITE, SET
  PARAMETERS and IDENTIFY. The real WD1003-WA2 also has SEEK (7xh), READ
  VERIFY (40h), FORMAT TRACK (50h) and DIAGNOSE (90h): INT 13h AH=04h
  verify, low-level format tools and POST all use them. IDENTIFY (ECh) is
  an ATA command a real WD1003 aborts. It stays because the Bochs BIOS's
  `ata_detect()` needs it, so label it. Source: WD1003-WA2 OEM manual.
- **S2. FORMAT TRACK leaves old sector data.** Same as the 486's S2.
  Source: NEC uPD765 data sheet, FORMAT A TRACK.
- **S3. The data rate is ignored.** Writes to 3F7h are dropped, so any
  rate reads any disk. On a real AT a 360KB disk in the 1.2MB drive reads
  only at 300 kbps, and the BIOS finds it by retrying rates. Source: IBM
  PC/AT Technical Reference, diskette adapter.

## 6. Keyboard and RTC

- **K1. Bytes can be lost.** `push_output()` overwrites a byte the BIOS
  hasn't read yet, and each `sendKey` in `app.js` runs its own 20 ms
  timer chain, so two keys' sequences can interleave. The 486 hit exactly
  this as stuck keys (§40.2). Queue on the keyboard side and send one
  sequence at a time.
- **K2. No typematic repeat.** The page forwards the browser's key
  repeats at the host OS rate. A real keyboard repeats on its own clock
  (500 ms, 10.9 cps at power-on) and `F3` changes it. Port §40.2 and drop
  the browser repeats. Source: IBM PC/AT Technical Reference, keyboard.
- **K3. Keyboard commands just ACK.** `ED` should ACK its LED byte too,
  `EE` answers `EE`, `F3` sets the rate (K2), `F5`/`F4` disable and
  enable scanning. Port from `pc486`'s `i8042.cpp`. Same source.
- **K4. Status bit 4 is inverted.** The AT's 8042 reports the front-panel
  keylock there, 1 meaning not inhibited. This code sets it when the
  keyboard is disabled ("clone convention"), the opposite. Same bug in
  `pc486`. The keylock itself, on the bezel, would be a nice period
  control. Source: IBM PC/AT Technical Reference, 8042 status register.
- **K5. Missing 8042 commands.** `C0` (read input port: keylock, display
  switch, the planar RAM jumper) and `E0` (test inputs) return nothing.
  Same source.
- **R1. The RTC doesn't tick.** DOS boots to a zero date and time, UIP
  never sets, IRQ8 never fires. Port §40.1: the MC146818A update cycle,
  periodic and alarm interrupts, loading the visitor's local time on
  power-on. Keep the AT's 64-byte part (the 486 went to 128 for the
  DS12887, which the AT doesn't have). Source: Motorola MC146818A data
  sheet.

## 7. Test parity

- **X1. Smoke doesn't prove a boot.** `web/tests/smoke.spec.ts` only
  checks real-speed pacing and says it "only needs the machine running".
  `CLAUDE.md` wants a boot plus one real interaction. Add a case: boot to
  `C:\>` and echo a typed key. Same as the 486's X1.
- **X2. Thin native suites.** pit 7, pic 7, machine 8, cmos 6, fdc 9,
  dma 9. P1-P4 and R1 grow pit, pic and cmos anyway. Machine needs IRQ
  routing and cascading through the whole board.
- **X3. Controls without a test.** `hddLed` has none, and the F-key and
  extra-key rows are only checked for enabled state and focus, not that
  they deliver a scan code. Every control needs one per `CLAUDE.md`.
- **X4. No coverage figures.** `make coverage` and `make -C web coverage`
  exist but the review has no recorded numbers. Record a baseline before
  this backlog starts, then refresh it.
- **X5. Review corrections.** §1 and §3 still describe protected mode
  as out of scope. Update them when C10 lands.

## Not ported from the 486, on purpose

VBE, the DAC and 9-dot text (VGA only), the cache model, FPU tags and
SoftFloat (no 80287 fitted; the ESC opcodes doing nothing is right), the
DX signature, #AC and debug registers (386 and later), the 15-byte limit
(the 286's is 10, §44.5), SB16/OPL3/CD-DA, and the PS/2 mouse (the AT has no
aux port; a serial mouse via P7 is the period way).

Already in line with the rules, for reference: the `?test=1&fast=1`
multiplier is gated in `app.js` and the smoke file keeps a real-speed
check; deploy CI runs `ibmpcat-test` / `ibmpcat-web-test` smoke only, and
the full suites run nightly.
