# Z80 core -- design notes

The NMOS Zilog Z80 shared by Pac-Man, Frogger, Scramble, Galaxian and
Galaga (`cpu_z80.{h,cpp}`). Items fixed from `Z80_ARCADE_PARITY.md` are
written up here: the fact, why it matters, what changed, and the source.

## 1. Sources

- **Zilog Z80 CPU User's Manual (UM0080).** Instruction set, T-states,
  machine-cycle timing diagrams, interrupt modes, IFF table.
- **Sean Young, "The Undocumented Z80 Documented".** Undocumented opcodes
  and flags, MEMPTR, power-on register values, the LD A,I/R parity quirk.
- **Patrik Rak (2018).** The Q register behind SCF/CCF's X/Y on NMOS parts.
- **David Banks (hoglet67), Z80Decoder (2018).** Flags and MEMPTR for
  interrupted LDIR/CPIR/INIR/OTIR, found by logic-analyser capture.
- **Tony Brewer, "Z80 Special Reset" (2014)**, confirmed on hardware with
  Mark Woodmass's HALT2INT (2021). What a halted CPU fetches.
- Cross-checks only, not behaviour sources: MAME `devices/cpu/z80/z80.lst`
  (microcoded M-cycles) and Manuel Sainz de Baranda y Goñi's redcode Z80.

## 2. Gates

- `make test-smoke`: named GoogleTests, `Z80.*` (instruction semantics) and
  `Z80Timing.*` (strobe positions, /WAIT, tick). Deploy CI.
- `make zexdoc` / `make zexall`: Frank Cringle's exercisers. zexall covers
  the undocumented X/Y flags. 67/67 groups each.
- `make sst`: SingleStepTests/z80, 1000 cases for each of 1604 opcode
  sequences. Each case checks every register, WZ, Q, the EI and LD A,I/R
  latches, RAM, port traffic, the T-state count, and the T-state, kind,
  address and data of every memory and I/O strobe plus the refresh
  address. The cases were generated from the Ares core, so they're a
  cross-check, not silicon. 1,604,000/1,604,000.

zexall and SST run nightly (`make check`).

## 3. T-state exact bus (parity Z6)

**Fact.** A Z80 instruction is a sequence of machine cycles: a 4 T M1
fetch (5 or 6 T on some opcodes), 3 T memory reads and writes, 4 T I/O
cycles with one automatic wait, and internal cycles. The /WAIT pin holds
any of them at T2 (TW for I/O). /INT is a level sampled at the end of
each instruction. /NMI is an edge, latched and taken at the next
instruction boundary.

**Why it matters.** A board sees each access at a specific T-state. The
core used to perform every access of an instruction at once, with no
/WAIT, so a board couldn't model video-RAM wait states (Pac-Man P6), beam
races, or a CPU held off the bus. It also had no /INT line, so a board
that fired `interrupt()` while IFF1 was clear lost the request.

**What changed.** Every instruction is written out as its UM0080
M-cycle sequence through `m1`/`mr`/`mw`/`ior`/`iow`/`tick`. `Cpu::cycles`
is current inside every bus callback. `Bus::wait` returns /WAIT T-states
for a cycle. `Bus::tick` reports T-states as they pass, so a board can
advance video or another CPU mid-instruction. `Bus::refresh` gets I*256+R
in M1 T3. `Cpu::set_int` holds /INT, and `Cpu::set_nmi` latches /NMI on a
rising edge. `step()` takes a pending NMI, then a held INT, then runs the
next instruction or halted M1. `interrupt()` and `nmi()` still accept
immediately, so boards that call them are unchanged. SST checks every
strobe position. `Z80Timing.*` pins read, write, I/O, indexed, push and
IM 2 positions, /WAIT on fetch, read, I/O and INTA, and tick totals.
`Z80.HeldIntIsTakenOnceInterruptsAreEnabled`, `Z80.NmiIsEdgeTriggered`.

INTA is an M1 with two automatic waits (6 T). IM 1/RST then spend one
internal T before the push, giving 13 T. IM 2 pushes, then reads the
vector, giving 19 T. The NMI acknowledge is a 5 T opcode fetch at PC with
the data discarded, then the push, giving 11 T.

## 4. Block I/O (parity Z1)

**Fact.** INI/IND/INIR/INDR and OUTI/OUTD/OTIR/OTDR are documented
instructions. INI puts BC (old B) on the address bus and stores to (HL).
OUTI decrements B first, then reads (HL) and outputs to BC. They take
16 T, or 21 T per repeat. Their flags come from B and from
`t = byte + (C±1)` (INI) or `t = byte + L` (OUTI): H and C are `t > 255`,
P/V is the parity of `(t & 7) ^ B`, and N is bit 7 of the byte (Young).
An interrupted repeat also shows PC bits 13/11 in Y/X and adjusts H and
P/V from B's next value (Banks).

**Why it matters.** All eight fell through to an 8 T NOP. A sound program
streaming register writes with OTIR would have silently done nothing.

**What changed.** `block_in`/`block_out`/`block_io_flags`/
`block_io_repeat_flags`. `Z80.Ini*`, `Z80.Ind*`, `Z80.Inir*`, `Z80.Outi*`,
`Z80.Outd*`, `Z80.Otir*`, and SST's `ed a2`-`ed bb`.

## 5. MEMPTR and Q (parity Z2)

**Fact.** MEMPTR (WZ) is an internal 16-bit register that many
instructions load as a side effect: an address+1, a jump target, IX+d,
BC+1 for I/O. It's visible only through BIT n,(HL), whose X/Y come from
MEMPTR's high byte. Q holds F after an instruction that wrote the flags,
and 0 otherwise. On NMOS Zilog parts SCF and CCF set X/Y from
`(Q ^ F) | A`. A DD/FD prefix is its own M1 and clears Q.

**Why it matters.** zexall and real software that probes the undocumented
flags read different values from the old core's plain "copy A" rule and
operand-based BIT.

**What changed.** `Cpu::wz` and `Cpu::q` are maintained for every
instruction (Young's MEMPTR table; Banks for repeats). `setf` marks
instructions that write F. POP AF and EX AF,AF' load F without marking it.
`Z80.BitHlTakesXyFromMemptr`, `Z80.Memptr*`, `Z80.Scf*`, `Z80.CcfUsesQ`,
`Z80.PopAfLeavesQClear`, `Z80.IndexPrefixClearsQ`. zexall passes.

## 6. R, refresh and reset (parity Z3)

**Fact.** R's low 7 bits count M1 cycles: every opcode and prefix fetch,
each halted M1, and the INT and NMI acknowledges. The DD CB d op opcode
byte is a plain read, not an M1. The refresh address is I*256 plus R
before the increment. After /RESET, AF and SP read FFFFh, PC, I, R, IFF
and IM are 0, and the rest are undefined (Young).

**Why it matters.** Games seed randomness from LD A,R, so a missing
increment per interrupt drifts the sequence from a real board.

**What changed.** `bump_r` in every M1 and acknowledge. `reset()` sets AF
and SP to FFFFh and leaves the other registers alone.
`Z80.InterruptAcknowledgeIncrementsR`,
`Z80.NmiAcknowledgeIncrementsRAndFetchesAtPc`,
`Z80.IndexedCbOpcodeByteIsNotAnM1`,
`Z80.RefreshCarriesIAndRBeforeIncrement`,
`Z80.ResetSetsAfAndSpToFfff`.

## 7. 16-bit port addresses (parity Z4)

**Fact.** IN A,(n) and OUT (n),A put A on A8-A15. The (C) forms and block
I/O put B there.

**What changed.** `Bus::in`/`out` take `uint16_t`. Every board here
decodes only A0-A7, so each casts the port to 8 bits. Pac-Man's vector
latch matches `(port & 0xFF) == 0`. `Z80.InANPutsAOnHighByte`,
`Z80.InRCPutsBOnHighByte`, `Z80.OutAToPort`.

## 8. IM 0 (parity Z5)

**Fact.** In IM 0 the CPU executes whatever instruction the interrupting
device drives, with PC frozen. The first byte comes in the INTA cycle,
which is two T-states longer than a normal M1 (UM0080). RST is 13 T and
CALL nn is 19 T.

**Why it matters.** The old core took `data & 0x38` as an RST target for
any byte.

**What changed.** `accept_int` runs the byte through the normal decoder
with `from_bus_` set, so opcode and operand fetches read `Bus::irq_data`
instead of memory. `Z80.Im0ExecutesCallFromBus`,
`Z80.Im0ExecutesOneByteInstructionWithoutMovingPc`,
`Z80.Im0JamsRstFromBus`.

## 9. Smaller corrections found on the way

- **NMI leaves IFF2 alone.** UM0080's IFF table: NMI resets IFF1 and
  leaves IFF2, so RETN restores the pre-NMI state. The old core copied
  IFF1 into IFF2, which loses the outer state on a nested NMI.
  `Z80.NmiVectorsTo66AndLeavesIff2`.
- **LD A,I/R parity quirk.** On NMOS parts an interrupt accepted right
  after LD A,I or LD A,R clears P/V (Young).
  `Z80.InterruptAfterLdAIClearsParity`.
- **HALT.** HALT advances PC. Each halted M1 fetches and discards the byte
  after HALT without moving PC (Brewer; HALT2INT).
  `Z80.HaltRefetchesTheByteAfterHalt`.
- **OUT (C),0.** The undocumented ED 71 drives 0 on NMOS parts.
  `Z80.OutCZeroDrivesZeroOnNmos`.

## 10. Cost

The M-cycle core runs zexdoc about 1.6x slower than the old
instruction-atomic one (70 s vs 44 s on the dev machine). That still comes
to about 660 MHz of emulated Z80 per host core, against 9.2 MHz for
Galaga's three CPUs. Every arcade's real-time pacing smoke test passes.

## 11. How the boards use it

Every main CPU drives its video beam from `Bus::tick`, so a write lands
on the pixel the beam has reached. Vblank interrupts are held lines
(`set_int`, or `set_nmi` held until the enable latch clears), matching
each board's flip-flop. The Konami sound CPUs hold /INT until the
acknowledge (`irq_data` clears it). Galaga's 06XX and sound NMIs are
edges. `Bus::wait` is unused: no board has a sourced wait state yet
(Pac-Man P6 in `Z80_ARCADE_PARITY.md`).
