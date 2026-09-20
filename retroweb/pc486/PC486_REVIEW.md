# 486DX2-66 Gaming PC -- build review log

This machine is not a recreation of a specific historical model, the way
`ibmpc-at` recreates a genuine IBM 5170. It's a "gamer's dream" 486-class
PC as a well-appointed enthusiast might have assembled/bought in 1993-94:
a real Intel 80486DX2-66 (33MHz bus, clock-doubled to 66MHz internally,
on-die FPU), 32MB RAM, a 504MB IDE hard disk (the genuine pre-EIDE INT13h
CHS addressing ceiling -- 1024 cyl x 16 head x 63 sec -- that period
"maxed out" drives actually ran into), a single 3.5" 1.44MB floppy, a 2x
ATAPI CD-ROM, VGA mode 13h with VESA BIOS Extensions, a PS/2 mouse, and a
Sound Blaster 16.

Milestone 1 (real-mode CPU, chipset, floppy, CD-ROM, HDD, basic VGA text/
graphics, booting to a FreeDOS/MS-DOS prompt), Milestone 2 (protected mode,
paging, task switching, the FPU -- §6), Milestone 3 (VGA mode 13h and the
VESA BIOS Extensions -- §7), and Milestone 4 (a PS/2 mouse and a Sound
Blaster 16 -- §10-§12) are all complete; BOOM, a real DJGPP/CWSDPMI DOS
Doom source port, runs and renders through the whole stack (§9).

This doc follows the same discipline as `ibmpc-at/IBM_PCAT_REVIEW.md`:
each section is a real design decision or bug, written up as fact ->
why it matters -> what it fixed, with citations to source material
(Intel manuals, ATA/ATAPI/SCSI-3 MMC specs, period drive datasheets)
wherever a specific quirk is being preserved rather than guessed at.

## 1. ISA-standard chipset devices reused from ibmpc-at

A real 486 gaming PC of this era used the same programming-model ISA
parts as the AT -- 8259 PIC, 8253/8254 PIT, 8237 DMA, 8042 keyboard
controller, CMOS RTC -- almost always integrated into a Super I/O chip by
1993-94 rather than discrete parts, but software-identical to the AT's
own discrete implementation. `pic8259`, `pit8253`, `dma8237`, `i8042`,
`cmos_rtc`, and `pcspeaker` are adapted from `ibmpc-at` (namespace/doc
references only -- no behavioral changes) on exactly that basis: this is
genuine period hardware-compatibility reuse, not a shortcut.

## 2. Floppy: single 3.5" 1.44MB bay

`fdc765` is adapted from ibmpc-at's two-drive (1.2MB/360KB, 5.25")
implementation to this machine's single 3.5" 1.44MB drive (80 cyl/2
head/18 sec/track, 500 kbit/s, 300 RPM), matching the user's explicit
"1990s period-correct 3.5 inch floppy" requirement -- singular, no second
drive. The drive also accepts genuine double-density 720KB 3.5" media (80
cyl/2 head/9 sec/track, 250 kbit/s), the same real "one physical drive,
two densities" fact ibmpc-at's A: already modeled for 1.2MB/360KB.

## 3. Storage: a 504MB IDE disk and an ATAPI CD-ROM, on two channels

A period-correct 1993-94 gaming PC has two IDE channels: the hard disk on
the primary (0x1F0-0x1F7 / 0x3F6 / IRQ14) and the CD-ROM on the secondary
(0x170-0x177 / 0x376 / IRQ15), separate cables and separate controllers
sharing one register-layout convention. That split is not cosmetic -- a 2x
CD-ROM's ~250 ms access time would otherwise hold the shared cable and stall
every disk request queued behind it, which is exactly why boards of this era
cabled them apart. `wd1003.h`/`.cpp` (adapted from `ibmpc-at`) is the
primary channel; `atapi_cdrom.h`/`.cpp` is new work for the secondary.

Specs worked from: **ATA/ATAPI-4** (NCITS 317-1998, X3T13/1153D) for the
packet protocol and register semantics; **ATA/ATAPI-6** (T13/1410D revision
3a) for the one rule ATA-4 left under-specified (§3.4 below); **SCSI-3 MMC**
(NCITS 304) and **SFF-8020i** ("ATA Packet Interface for CD-ROMs") for the
command set; **SPC** (NCITS 301) for the commands MMC inherits from the
general SCSI command set. Behavior was additionally checked against the real
DOS-side consumer, FreeDOS `ATAPICDD.SYS` (`FDOS/cdrom`,
`driver/atapicdd/atapicdd.asm`), since that driver -- not a spec -- is what
will actually probe this device at boot.

### 3.1 The 504MB geometry is the CHS ceiling, not a round number

`Wd1003::mount()` changed from the AT's ST-4038 (733 cyl / 5 head / 17 sec)
to **1024 cyl / 16 head / 63 sec = 1,032,192 sectors = 528,482,304 bytes =
exactly 504MB**. That specific number is a genuine, well-documented period
artifact worth preserving deliberately rather than picking a tidy capacity:
INT 13h packs the cylinder into 10 bits (max 1024) and the sector into 6
bits (max 63, 1-based), while the ATA task file allows 65536 cylinders but
only 16 heads. Neither limit alone bites; their *intersection* -- 1024 × 16
× 63 -- is the largest geometry classic CHS translation can express at all,
which is why "maxed out" pre-EIDE IDE drives of 1993-94 landed on 504MB and
why EIDE LBA translation was introduced to break it. A drive one cylinder or
one sector larger is unreachable by this addressing, not merely inefficient.

Changed with the geometry: `mount()`'s constants and its citation comment,
the IDENTIFY model string (`RETROWEB IDE 504MB`, since the old string named
the ST-4038 explicitly), and `wd1003.h`'s header note that
`Machine::configure_factory_cmos()`'s legacy CMOS "Type 47" geometry bytes
must describe this same geometry -- the BIOS reads the CMOS table and the
IDENTIFY response through two separate code paths, and they have to agree or
they describe two different drives. Everything else about the controller is
untouched: same register model, same paced-read/synchronous-write
asymmetry, same device-selection gating. `tests/wd1003_test.cpp` is new here
(the file did not exist for pc486 yet), mirroring ibmpc-at's structure, with
its geometry assertions rewritten and one added test pinning the capacity
arithmetic and both CHS field limits.

One test-hygiene note: the test images are deliberately truncated prefixes
of the real 504MB capacity (a few MB), because the controller bounds-checks
requests against the mounted image's actual length while IDENTIFY reports
`mount()`'s fixed constants -- so geometry coverage is unaffected, and no
test has to allocate half a gigabyte.

### 3.2 ATAPI device architecture

`AtapiCdrom` is the sole device on the secondary channel (device 0/master,
2x, tray loader), with removable media (`mount()`/`eject()`/
`media_present()`) like `fdc765`'s floppies rather than `wd1003`'s fixed
disk. The public surface is deliberately the same shape as `wd1003`'s
(`reset`/`owns`/`in`/`out`/`data_in16`/`data_out16`/`tick`/`irq_pending`/
`busy`) so the chipset decodes and cascades it identically.

The PACKET protocol (ATA/ATAPI-4 §9.6) runs as four phases: the host writes
a per-block byte-count limit and issues PACKET (0xA0); the device raises DRQ
with Interrupt Reason = 01h (C/D=1, I/O=0) to request the 12-byte CDB; the
CDB arrives as six 16-bit words through the data register; the device
asserts BSY, executes, then hands the result back as one or more DRQ data
blocks with Interrupt Reason = 02h (I/O=1, C/D=0) and the block's real size
published in the byte-count registers, finishing with Interrupt Reason = 03h
("command complete") and DRQ clear. Data transfers are inherently 16-bit,
so the data register needs the same genuinely-atomic path as the primary
channel's 0x1F0 (0x171 is the Error/Features register, not "the high byte of
0x170").

Transfer pacing follows the same tradeoff `wd1003.h` and `fdc765.h` already
document -- respect the real total transfer *time*, not the byte-by-byte bus
handshake -- via one accumulated cycle-credit target in `tick()`. The rates
are a real 2x drive's: 1x is 75 sectors/sec × 2048 bytes = 153,600 bytes/sec,
so 2x is exactly 307,200 bytes/sec (not a rounded "300 KB/s"), plus real access
time on anything the drive's read-ahead buffer cannot serve. Period
Mitsumi/Sony/Matsushita 2x datasheets quote 250-350 ms *average* access, and
"average" there means a one-third-stroke seek -- so access time is modelled as
a 64KB read-ahead buffer (requests landing inside it cost nothing but transfer)
plus a distance-dependent seek calibrated so a one-third-stroke move costs
exactly that published 250 ms. This is why sequential CD reads feel an order of
magnitude faster than seeky ones, a difference a flat per-command latency would
erase -- and charging that flat average to *every* non-contiguous read turned
out to make the drive slower than the real thing, which is a real bug this
machine hit and fixed (§5.7). Per CLAUDE.md there is no speed override:
`Read10IsPacedToRealTwoSpeedDriveTiming` asserts a one-sector read *cannot*
complete earlier than real hardware could deliver it.

### 3.3 Detection: the signature is the whole mechanism

The secondary channel's register block is byte-for-byte identical to the
primary's, so "is this a disk or a CD-ROM?" is answered entirely by what the
device puts in the task file after a reset. ATA/ATAPI-4 §9.1 ("Signature and
persistence") specifies Sector Count = 01h, Sector Number = 01h, Cylinder
Low = **14h**, Cylinder High = **EBh** for a packet device, against
01h/01h/00h/00h for an ATA disk. `ATAPICDD.SYS` implements literally that
comparison (`cmp AX, 14EBh` after reading CL/CH), as does the Bochs BIOS's
`ata_detect()`, so this is the single most load-bearing fact in the file.

Two further pieces of the same mechanism are implemented rather than
approximated:

**Status reads 00h after reset -- DRDY is deliberately clear.** §9.1's
signature table gives a packet device Status = 00h, and a packet device
never reports itself "ready" the way a disk does. Software that waits for
DRDY on a CD-ROM waits forever, which is precisely why ATAPI drivers poll
BSY instead. Setting DRDY here "to be helpful" would also let a host's ATA
branch (`cl==0 && ch==0 && st!=0`) misclassify the drive.

**IDENTIFY DEVICE (0xEC) is aborted, with the signature placed in the task
file** (ATA/ATAPI-4 §8.12.1) -- the second half of detection, for a host
that issues the ATA identify before checking the reset signature. It returns
ABRT with the sense key in the Error register's high nibble and no data,
instead of a plausible-looking all-zero geometry. `IDENTIFY PACKET DEVICE`
(0xA1) is the command that actually answers, with ATAPI's own field layout:
word 0 = 0x85C0 (bits 15:14 = 10b ATAPI, bits 12:8 = 05h CD-ROM command
set, bit 7 removable, bits 6:5 = 10b accelerated DRQ, bits 1:0 = 00b 12-byte
packet), word 49 advertising LBA but **not** DMA, word 63 no multiword DMA
modes, words 82-85 declaring the PACKET and DEVICE RESET feature sets.

### 3.4 "Device 0 responding for Device 1": where this device correctly disagrees with `wd1003`

`wd1003` models its permanently-absent slave with a two-read window in which
Cylinder Low/High float to 0xFF/0xFF, tuned to the Bochs BIOS's probe. The
obvious move was to copy that. Reading the actual spec text says otherwise,
and the difference turned out to matter.

**ATA/ATAPI-6 (T13/1410D revision 3a), Table 18, "Device 1 is selected and
Device 0 is responding for Device 1"** (which folded in proposal E00118R0's
ATAPI half) specifies this case completely, and specifies it *differently
for a packet device than for a disk*:

- Reads of Sector Count, LBA Low/Mid/High and the Device register: "If the
  device does not implement the PACKET Command feature set, the device shall
  place the contents of the Device 0 ... register on the data bus. If the
  device implements the PACKET Command feature set, the device shall place
  00h on the data bus."
- Reads of Status and Alternate Status: "Place 00H on the data bus" for
  either kind of device.
- The Error register still reports Device 0's real contents.
- Every register *write* lands in Device 0's register, command register
  included -- "Place new data into the Command register of Device 0. Do not
  respond unless the command is EXECUTE DEVICE DIAGNOSTICS."

So `wd1003`'s ungated writes and its contents-of-device-0 reads are exactly
right *for a disk*, and 00h is exactly right *for this device* -- the two
implementations are not inconsistent, they are two rows of the same table.

This is load-bearing rather than pedantic. `ATAPICDD.SYS` probes device 1
twice: first by writing 0x55/0xAA to Sector Count/Number and requiring the
read-back, then (after a reset) by requiring Sector Count/Number = 01h/01h
before it even looks for the 14h/EBh signature. Handing back this device's
own shared-latch values would pass *both* probes and then return the real
master's 14h/EBh -- inventing a phantom ATAPI slave that the driver would
register and then time out against on every access. The floated-0xFF model
would pass the first probe too. Only the spec's 00h answer makes the real
driver conclude, correctly, that nothing is there.

The EXECUTE DEVICE DIAGNOSTIC exception is implemented as stated (it is how
a host collects a diagnostic result covering both device positions from a
single-device channel), and a soft reset issued while device 1 is selected
still resets device 0, per the same table's Device Control row.

### 3.5 Shared latches at offsets 2-5

The register at offset 2 is one physical latch that the host writes as
Sector Count (unused by ATAPI) and reads as the **Interrupt Reason**
register; offsets 4/5 are likewise written as the host's per-block **byte
count limit** and read back as the **actual byte count** of the current
data block. The device only overwrites them at a phase transition. This is
not a modeling convenience: `ATAPICDD.SYS`'s (and the Bochs BIOS's) device
probe writes 0x55/0xAA to offsets 2/3 and requires them to read back before
it will attempt anything else, and that probe works on real drives precisely
because these are shared latches. Modeling offset 2 as a read-only phase
register would make this device invisible to its own driver.

A related preserved quirk: an **odd byte-count limit drops to the next even
value** (ATA/ATAPI-4 §7.3.2) -- the classic case being a host that writes
0xFFFF and gets 0xFFFE. `ATAPICDD.SYS` does exactly that, and says so in its
own source (`mov AX, 0FFFFh ; set cyl value to max transmit size per PIO DRQ
transfer (==0xFFFE)`). A limit of zero is treated as "no limit" rather than
deadlocking a driver that never set one.

Two more real ATAPI register facts, easy to get wrong by analogy with the
disk: the **Error register on a packet device carries the SCSI sense key in
bits 7:4** (with MCR/ABRT/EOM/ILI in the low nibble), not a disk's
IDNF/UNC bit field (ATA/ATAPI-4 §7.6.1); and **Status bit 4 is SERVICE and
bit 5 is DMA-READY**, not the disk's DSC (ATA/ATAPI-4 §7.15.6.3). `wd1003`
sets 0x10 (DSC) on every completion; this device deliberately never does,
because here that bit would falsely claim a service request is pending for
an overlapped command.

### 3.6 The command set: which CDBs, and why exactly these

Implemented (all 12-byte CDBs; SCSI multi-byte fields are big-endian per SPC
§3.4.2):

| CDB | Command | Why |
| --- | --- | --- |
| 00h | TEST UNIT READY (SPC §7.25) | The universal "is there a disc?" poll; every driver's media loop starts here. |
| 03h | REQUEST SENSE (SPC §7.20) | Carries the sense key/ASC/ASCQ that make every other failure legible. |
| 12h | INQUIRY (SPC §7.5) | Device-type identification (peripheral type 05h, RMB set); succeeds with an empty tray. |
| 1Bh | START STOP UNIT (MMC §6.1.13) | Eject/load -- the tray, from the software side. |
| 1Eh | PREVENT ALLOW MEDIUM REMOVAL (SPC §7.12) | Drivers lock the tray before reading; refusing an eject while locked is real behavior. |
| 25h | READ CAPACITY (MMC §6.1.10) | Reports last LBA + the 2048-byte block size a DOS driver must translate against. |
| 28h | READ(10) (MMC §6.1.7) | The actual data path, 2048-byte sectors. |
| 2Bh | SEEK(10) (MMC §6.1.15) | `ATAPICDD.SYS` issues it for the DOS seek device command. |
| 43h | READ TOC/PMA/ATIP (MMC §6.1.12) | `ATAPICDD.SYS` reads the TOC during its disc-present/disc-type check. |
| 5Ah | MODE SENSE(10) (MMC §6.1.6 / SPC §7.10) | The page 2Ah capabilities probe: speed, loader type, eject/lock support. |

The brief's required seven are all present; SEEK(10), READ TOC and PREVENT
ALLOW MEDIUM REMOVAL were added because reading the real driver showed it
issuing them on paths that would otherwise fail. The single-session,
single-track MODE1 data disc these images always are makes the TOC exactly
track 1 plus the lead-out; MSF addressing carries the Red Book 2-second (150
frame) pregap, so LBA 0 reports as 00:02:00 -- a driver that treats an MSF
address as an LBA without removing the pregap lands 150 sectors off.

Deliberately **not** implemented, each for a reason rather than as a gap:

- **MODE SENSE(6)/MODE SELECT(6) (1Ah/55h)** -- SFF-8020i defines only the
  10-byte forms, so a real ATAPI CD-ROM rejects the 6-byte ones with INVALID
  COMMAND OPERATION CODE and drivers probe with them precisely to learn they
  must use the 10-byte form. Refusing them is the accurate answer.
- **GET EVENT STATUS NOTIFICATION (4Ah)** -- an MMC-2 (1997) command that a
  1993-94 2x drive genuinely predates. `ATAPICDD.SYS` asks for it first in
  `getMediaStatus()` but handles refusal explicitly (its
  `@@assumeNotSupported` path returns `STATUS_MEDIA_UNKNOWN`) and falls back
  to polling TEST UNIT READY -- which is what a real period drive forced it
  to do. Implementing it would be an anachronism that changes driver
  behavior, not a convenience.
- **Audio playback (PLAY AUDIO, READ CD-DA)** -- not implemented, and the
  page 2Ah capabilities byte honestly reports no audio support rather than
  advertising a feature whose CDBs would then fail.
- **DMA** -- the PACKET command's Features-register DMA bit is aborted with
  ABRT rather than silently falling back to PIO, so a driver's negotiation
  notices; IDENTIFY advertises no DMA modes for the same reason. There is no
  bus-master controller on this machine to program.
- **Data-out CDBs** -- this is a read-only drive; no implemented command
  transfers data to the device. Unknown opcodes get a genuine CHECK
  CONDITION / ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE.

### 3.7 Unit attention is the media-change mechanism

Per SPC §5.6 a unit-attention condition is raised on reset (sense key 06h,
ASC 29h "power on, reset, or bus device reset occurred") and on every media
change (ASC 28h "not ready to ready change, medium may have changed"), and
is reported as CHECK CONDITION on the next command *other than* INQUIRY or
REQUEST SENSE, then cleared. That exemption matters: a host has to be able
to identify the drive and read the sense data without the report getting in
the way. REQUEST SENSE itself therefore never fails -- including with an
empty tray, since it is the command a driver uses to find out *why*
something failed -- and reading the sense data clears it back to NO SENSE
(SPC §7.20).

The consequence most likely to surprise anyone integrating this: **the first
command after every reset returns CHECK CONDITION**, with 06h/29h/00h. Real
drives do exactly this, and it is why DOS CD-ROM drivers issue TEST UNIT
READY twice at startup. It is genuine, not a bug, but it is the behavior a
driver author hits first.

With a disc absent, the standing condition after the change has been
reported is NOT READY / MEDIUM NOT PRESENT (02h/3Ah/00h) for every
media-requiring command; INQUIRY and MODE SENSE(10) keep working, because
they describe the drive rather than the medium. The host-side `eject()` (a
front-panel button -- the user physically taking the disc, the real-world
equivalent of the emergency eject hole) deliberately overrides a driver's
PREVENT ALLOW MEDIUM REMOVAL lock; the START STOP UNIT path honors it and
returns ILLEGAL REQUEST / MEDIA REMOVAL PREVENTED (05h/53h/02h), as a real
drive does.

### 3.8 Tests

`tests/atapi_cdrom_test.cpp` (40 cases) drives the device only through
`in`/`out`/`data_in16`/`data_out16`, i.e. the real programming model the
chipset's port decode will use, matching `wd1003_test.cpp`'s convention.
Coverage: port decode; the post-reset ATAPI signature and 00h status; the
0x55/0xAA scratch probe through the shared latches; soft reset; Table 18's
00h responses for the absent device 1 and its EXECUTE DEVICE DIAGNOSTIC
exception; IDENTIFY PACKET DEVICE's field layout; IDENTIFY DEVICE's abort;
DEVICE RESET raising no interrupt; every implemented CDB including its
no-media and out-of-range failures; byte-count-limited multi-block PIO with
an interrupt per block; the odd-limit rounding; unit attention on reset and
across eject/re-insert; the lock/override pair; nIEN masking; and the
2x pacing floor. `tests/wd1003_test.cpp` (13 cases) is the adapted disk
suite described in §3.1.

### 3.9 Open items for the chipset/driver integration

- **Cycle units -- resolved.** `AtapiCdrom::kCpuHz` (66,000,000, the DX2-66's
  internal clock) assumes `tick()` is fed the CPU's internal cycle count.
  `wd1003.h` and `fdc765.h` inherited the AT's `kCpuHz = 8000000.0` unchanged
  from the copy this file's author started from; `Chipset::tick()` feeds all
  three devices the same `cpu_cycles` counter, which `Machine` advances at
  the real 66MHz clock (`machine.h`'s `kCpuHz`). Left as-is, `wd1003` and
  `fdc765` would have paced their transfers as if each tick were 8.25x
  further apart than it actually is, finishing floppy/HDD transfers 8.25x
  faster than real time -- a genuine CLAUDE.md no-speed-up violation. Fixed
  by changing both constants to `66000000.0` to match.
- **`wd1003`'s transfer rate is anachronistic but not a violation.** Its
  625,000 bytes/sec is a representative ST-506/412 MFM figure; period WD
  Caviar drives of this capacity class (e.g. the AC-2540, 516MB, Jan 1994)
  quote an 11.1MB/s PIO3 *interface* burst rate, with sustained
  drive-to-buffer media rate well below that -- typically in the 1-2MB/s
  range for a 3600-4500RPM drive of this era, per period reviews. Since
  625,000 B/s (~610KB/s) is already on the *slow* side of that range, this
  is a conservative inaccuracy, not a speed-up -- it doesn't violate
  CLAUDE.md's rule the way the cycle-unit bug above did, so it's left
  as a follow-up rather than chased further here. Revisit with a specific
  period drive's datasheet if a precise figure is ever needed.
- **The reset unit attention** (§3.7) is the behavior most likely to look
  like a bug during driver bring-up.

## 4. CPU core: a real-mode 80486DX2-66

> **Superseded in part by §6.** This section records the core as Milestone 1
> left it -- real address mode only. Milestone 2 made protected mode, paging,
> task switching and the FPU real, which inverts several decisions below
> (notably §4.1's no-op list, its SMSW/CR0 reasoning, and §4.3's segment-limit
> premise, the last of which §5.4 had already falsified). The sections are kept
> as written, because the reasoning is what explains the code's shape; §6.1
> lists exactly what changed and why.

`cpu80486.h`/`.cpp` is the Milestone 1 CPU. It follows the same shape as
`ibmpc-at/cpu80286.h` -- a `Bus` struct of `std::function` callbacks with no
back-reference to the chipset, big-switch opcode dispatch with `grp*`
helpers for the ModR/M-reg-selected groups, an `on_unimplemented`
diagnostic hook, and per-opcode cycle costs cited inline -- but the parts
below are genuinely different, and the differences are the point.

### 4.1 Real mode only, and what that excludes

> Superseded by §6.1: every opcode on the list below now does its real work,
> SMSW reports hardwired ET = 1, MOV CR0 honors PE, and the protected-mode-only
> members are #UD in real mode rather than no-ops.

Scope is real address mode exclusively: no protected mode, no paging, no
V86, no x87. FreeDOS and MS-DOS 6 never leave real mode, and the Milestone
1 plan defers the rest. The consequence is a specific, enumerated list of
opcodes that are **documented no-ops** rather than silent gaps -- each
consumes its ModR/M (and SIB/displacement, so instruction lengths stay
correct) and charges its real published cycle cost, then does nothing:
LGDT/SGDT/LIDT/SIDT/LLDT/SLDT/LTR/STR/ARPL/LAR/LSL/VERR/VERW/CLTS, LMSW,
MOV to/from CR0-CR3 and DR0-DR7 and TR6-TR7, INVD/WBINVD/INVLPG, and the
whole x87 ESC range 0xD8-0xDF. The header comment carries that list in the
same style as `cpu80286.h` lines 14-19.

Two of those are better than inert, because real mode has a genuinely
correct answer available:

- **SMSW** stores a real machine-status word: PE = 0 (this core is always
  in real mode) and ET = 0 (no coprocessor is emulated this milestone).
  Real-mode software does read this -- a DOS extender stub asking "am I
  already in protected mode?" -- and 0 is the true answer here, not a
  placeholder.
- **MOV CR0, r32** accepts the write but masks PE off, so a read-back
  keeps saying "still real mode" instead of claiming a mode switch that
  never happened. CR1-CR3 and DR0-DR7 round-trip as plain storage so
  software probing them isn't fed garbage.

`on_unimplemented` deliberately does **not** fire for any of these: they
are decided gaps, not discoveries. It fires only for genuinely
unrecognized opcodes, which is what makes it usable as the same
evidence-gathering tool ibmpc-at used to find its BIOS's 386-baseline
assumptions (`IBM_PCAT_REVIEW.md`).

The one gap that is a real behavioral departure rather than an unexecuted
instruction is **#GP(0) on a segment-limit violation** -- see §4.3.

### 4.2 32-bit registers are native here, not a concession

`cpu80286.h` stores AX/BX/... as `uint32_t` and documents it at length as a
*compatibility workaround*: a genuine 80286 has no EAX, no 0x66 prefix and
none of the 0x0F opcodes, and that core only has them because the prebuilt
BIOS it boots turned out to assume a 386+ baseline. None of that framing
applies here. An Intel486 has EAX/EBX/ECX/EDX/ESP/EBP/ESI/EDI, FS and GS,
the 0x66 and 0x67 prefixes, and the 0x0F escape space as architecture.
Real-mode DOS code uses them routinely -- DOS extender stubs, 32-bit-aware
memory managers, anything a 386-targeting compiler emits in 16-bit mode --
without ever entering protected mode. So the comments here describe
behavior, not an apology, and `eax`/`esp`/`eip`/`eflags` are the public
field names.

The sub-register write rules come with that: writing AX never disturbs
EAX's upper half, writing AL never disturbs AH. `cpu80486_test.cpp` pins
both down.

**FS and GS** are fully implemented -- the 0x64/0x65 override prefixes, MOV
to/from them, PUSH/POP FS/GS (0F A0/A1/A8/A9), and LSS/LFS/LGS (0F
B2/B4/B5).

One place where this core deliberately diverges from its sibling:
**PUSH SP/ESP**. From the 286 onward the pushed value is the register's
value *before* the decrement; the 8086 pushes the already-decremented one.
That asymmetry is the classic `push sp / pop ax / cmp ax,sp` runtime check
for 8086-vs-286+ ([OSDev CPU
Detection](https://wiki.osdev.org/User:ChosenOreo/CPU_Detection)).
`ibmpc-at/cpu80286.cpp`'s `push_reg()` implements it inverted -- it pushes
the decremented value and its comment asserts that is the 286 behavior.
This core implements the documented direction and tests it; the 286 core is
untouched (not this milestone's file), but it is worth a look when someone
next has that file open.

### 4.3 The 0x67 address-size prefix, SIB decoding, and the 64KB limit

`cpu80286.h` explicitly declines 0x67 ("deliberately NOT supported"),
because nothing that machine boots needed 32-bit *addressing*. This core
implements it for real: the full 32-bit ModR/M table, the SIB byte
(scale 1/2/4/8, index, base), `mod=00 rm=101` as disp32-with-no-base, SIB
index field 100 as "no index" (ESP can never be an index), SIB base 101
with mod=00 as disp32-with-no-base, and the ESP/EBP-base -> SS default
segment rule (Intel 80486 PRM, "Addressing Modes" and "Default Segment
Attribute"). 0x67 also selects ECX over CX as the LOOP/JCXZ counter.

**The segment-limit decision.** A 32-bit effective address can exceed
0FFFFh, and real mode's segment limit is a fixed 64KB. On real hardware one
of two things happens: the CPU raises #GP(0), or -- if an "unreal mode"
setup has already loaded a descriptor with a larger limit -- the access
simply succeeds. The second is unavailable here by construction: expanding
a limit requires a protected-mode descriptor load, and this core has no
protected mode at all, so the 64KB limit is unconditional.

This core **wraps the offset into the low 16 bits**, exactly as the 16-bit
addressing path already wraps mod 64KB, and lists the missing #GP(0)
alongside the other protected-mode omissions in the header comment. The
reasoning: the fault is protected-mode machinery in a core that has none;
DOS installs no handler for vector 13, which in the real-mode IVT is IRQ5's
slot, so a "faithful" fault would vector into the IRQ5 handler and produce
chaos rather than fidelity; and an out-of-range EA in practice means a
decoding bug in this core rather than genuine guest behavior. Wrapping
keeps that visible and debuggable instead of scattering spurious
interrupts. It is labelled as a gap, not presented as correct.
`Addr32EffectiveAddressAboveSixtyFourKWrapsIntoTheSegment` pins the
decision down so it can't drift silently.

**LEA is the exception, and it matters.** `RM` carries both `off` (the
64KB-limited memory offset) and `ea32` (the untruncated 32-bit sum), and
LEA reads `ea32`. LEA touches no memory, so a 32-bit addressing form there
is pure arithmetic -- the "LEA as a three-input adder" idiom every 386+
compiler emits. Truncating `LEA EAX,[EBX+ECX*4]` to 16 bits would produce a
wrong *number*, not just a wrong address. Tested.

For the same "real mode's stack segment is always 16-bit" reason, pushes
and pops move SP and wrap mod 64KB while leaving ESP's upper half alone,
even for the 32-bit push/pop forms (which move SP by 4). There is no
descriptor B bit that could make it otherwise.

### 4.4 The AC flag (EFLAGS bit 18)

Implemented as genuine storage, because it is a real period detail with a
primary-source citation. Intel's AP-485, *Intel Processor Identification
and the CPUID Instruction*, says in its "Intel386 processor check":

> The AC bit, bit #18, is a new bit introduced in the EFLAGS register on
> the Intel486 processor to generate alignment faults. This bit cannot be
> set on the Intel386 processor.

and its sample code flips AC through PUSHFD/POP/XOR/PUSH/POPFD/PUSHFD and
branches on "can't toggle AC bit, processor=80386". Before CPUID existed on
486 steppings that was *the* way software told a 386 from a 486, and a
period "gamer's dream" machine plausibly runs something that probes this
way.

So: `kPopfdMask` (0x00047FD5) lets AC round-trip through POPFD/PUSHFD; the
16-bit POPF cannot touch it (it lives above bit 15), which is precisely why
the AP-485 sequence needs the 32-bit forms. Alignment *faults* themselves
require CR0.AM and CPL 3, neither of which exists in real mode, so the bit
is observable-but-inert here exactly as on real hardware.
`AcBitTogglesAndReadsBackPerAp485` runs the AP-485 sequence as written.

Adjacent decision: **POPF/POPFD/IRET load IOPL and NT unconditionally**,
which is genuine real-mode behavior (no CPL exists to gate them).
`ibmpc-at/cpu80286.cpp` deliberately masks those to zero because letting
them round-trip reproducibly broke the FreeDOS 1.3 installer on that
machine, root cause unconfirmed (`IBM_PCAT_REVIEW.md`). This core
implements the genuine behavior, since there is no evidence of that failure
here and every other 486 emulator does the same -- but the POPF call site
carries a comment saying so, and it is the first thing to bisect if a
FreeDOS oddity ever shows up on this machine.

### 4.5 CPUID is absent, and that is the accurate answer

**Superseded by §13.1.** The part this machine carries is an SL-Enhanced
IntelDX2-66, which does have CPUID, and the "absent" choice below cost the
machine its mouse driver, APM and CD-ROM support: FreeDOS's own `VINFO`
identifies the CPU with AP-485's ID-flag/CPUID sequence, and `FDAUTO.BAT`
runs `CTMOUSE` only on the 386+ branch that answer selects. The reasoning
recorded here is kept because it is right about the *earlier* part.

The DX2-66 modelled here predates CPUID. AP-485 states plainly:

> Older versions of Intel486 SX, Intel486 DX and IntelDX2 processors do not
> support the CPUID instruction

-- only the later SL-Enhanced / Write-Back-Enhanced 486 parts have it, and
the timing table used for cycle costs throughout this core lists CPUID as
"(Pentium+)". So `0F A2` takes the ordinary unrecognized-opcode path and
fires `on_unimplemented`, rather than returning invented identification
data. Software that probes with CPUID falls back to the AC-flag technique
above, which works -- the historically correct outcome for this part, not a
workaround.

### 4.6 Cycle costs

> Extended by §6.8, which adds the protected-mode control-transfer rows and the
> x87 rows under the same cited-number-versus-labelled-model discipline.

Costs are the 486 column of the Quantasm *80x86 Integer Instruction Set
(8088 - Pentium)* table, which reproduces Intel's own i486 PRM timing
appendix, cited inline per instruction. Four things are worth recording:

1. **No `+m` term.** `cpu80286.h` needs `kQueueRefillTax` because every 286
   taken-branch entry is a *range* driven by prefetch-queue refill, and the
   table's `+m` appears in the 286 and 386 columns. The 486 column
   publishes flat numbers for control transfers (Jcc taken 3, JMP near 3,
   CALL near 3, RET 5, IRET 15), so this core needs no equivalent tax and
   charges the published figures directly.
2. **The one published addressing-mode adjustment is implemented.** The
   table's legend gives, for the 286-486, "base+index+disp = +1, all
   others, no penalty". `decode_modrm()` adds exactly that -- which is also
   what makes LEA land correctly on its published "1-2".
3. **Genuine generational differences are preserved, not inherited from the
   286 core.** The on-chip cache makes MOV 1 clock in every direction (the
   286's directional 3/5 asymmetry is gone); the barrel shifter makes
   SHL/SHR/SAR/ROL/ROR count-*independent* (the 286 charged 5+n); the ALU
   group is not uniform (CMP/TEST read memory at 2, ADD and friends
   read-modify-write at 3); and IN/OUT are 14/16 against the 286's 5/3. A
   copied 286 table would have gotten every one of these wrong.
4. **Four families publish a data-dependent range, and each is a labelled
   model fitted to both published endpoints** -- not an invented constant,
   and not a claim of pipeline-accurate simulation:
   - **MUL/IMUL** (13-18 / 13-26 / 13-42 for 8/16/32-bit): the 486 uses an
     early-out multiply whose cost depends on the multiplier's
     most-significant-bit position. Intel publishes the mechanism and both
     endpoints but no per-bit formula, so `mul_cost()` charges
     13 + max(0, msb - 3), capped at 13 + (width - 3), reproducing 18/26/42
     exactly at the top and 13 at the bottom.
   - **BSF/BSR** (6-42 / 6-103): both scan a bit at a time, so
     `bitscan_cost()` charges the published floor plus one clock per bit
     actually examined.
   - **RCL/RCR by a count** (8-30 register, 9-31 memory): still iterative
     unlike the plain shifts, so `rotate_carry_cost()` is linear in the
     count, anchored so count 1 gives the floor and it saturates at the
     ceiling.
   - **CMPXCHG against memory** ("7-10"): charged 7 for compare-only and 10
     for compare-plus-store, the only split the instruction has. Intel
     prints the range without saying which end is which, so that is a
     labelled reading.

   Two smaller labelled inferences: REP INS/OUTS have no published 486 REP
   formula, so the published per-iteration 17 is charged per iteration; and
   prefixes cost 1 clock each, which is the published LOCK figure applied
   uniformly (the table publishes no separate segment-override /
   operand-size / address-size prefix cost).

`interrupt()` charges INT3's published 26 -- same IVT work, no immediate to
fetch -- and a private `do_interrupt()` performs the frame push and
vectoring without touching the cycle counter, so step()'s INT/INT3/INTO and
fault paths each charge their own published cost exactly once.
(`cpu80286.cpp` bills both: its `interrupt()` adds 45 to `cycles` *and*
step() adds its own 23+tax for the same instruction.)

### 4.7 Tests

> Superseded by §6.10: the suite is now 389 cases across six fixtures. The
> figures below are Milestone 1's.

`tests/cpu80486_test.cpp` -- 80 semantic cases against a mock `Bus`,
weighted toward what is new: the 32-bit register file and sub-register
write rules, PUSHAD/POPAD, FS/GS and their override prefixes, LSS/LFS/LGS,
the whole SIB/0x67 matrix (base+index*scale, disp32-no-base, scaled index
with no base, ESP/EBP -> SS defaults, JECXZ), the limit-wrap decision, LEA
keeping its full 32-bit result, BSWAP/XADD/CMPXCHG (both branches),
MOVZX/MOVSX, BSF/BSR including the undefined-destination-on-zero contract,
two- and three-operand IMUL, 32-bit shifts, SHLD/SHRD, the BT group
including memory bit-offsets beyond the operand width, the AC flag and the
AP-485 sequence verbatim, and that every protected-mode and FPU no-op stays
silent while an undefined opcode fires `on_unimplemented` (CPUID did too
until §13.1 implemented it).

`tests/cpu80486_timing_test.cpp` -- 49 cycle-cost cases, for the same
reason ibmpc-at keeps its cost model in a separate suite: it regresses
silently and only surfaces later as period software measuring the wrong CPU
speed. Covers the ALU read-vs-read-modify-write split, the
effective-address penalty, all four data-dependent range models at both
endpoints, the count-independence of the plain shifts against RCL/RCR's
saturation, every REP string formula including the published "5 if n=0, 13
if n=1" special cases, LOOP/JCXZ per-op-per-outcome costs, control
transfers, the no-double-billing property of `interrupt()`, the per-prefix
clock, and that the documented no-ops still carry their real published
figures.

Writing the suites found one real bug: ARPL (0x63) had been placed in the
0x0F escape space instead of the one-byte opcode table, so it fell through
to `on_unimplemented` instead of being a documented no-op. Fixed, with the
test that caught it.

### 4.8 Sources

- Intel 80486 Programmer's Reference Manual (1990/1992) -- instruction
  semantics, addressing modes, EFLAGS layout, flag-affected tables.
- Intel 8086/8088 User's Manual -- the shared 8086-legacy subset.
- Intel AP-485, *Intel Processor Identification and the CPUID Instruction*
  -- the AC-bit 386-vs-486 check and the IntelDX2 CPUID-availability
  statement. ([mirror](https://datasheets.chipdb.org/Intel/x86/CPUID/24161817.pdf))
- Quantasm, *80x86 Integer Instruction Set (8088 - Pentium)* -- the 486
  cycle-count column, reproducing Intel's i486 PRM timing appendix.
  ([mirror](https://www2.math.uni-wuppertal.de/~fpf/Uebungen/GdR-SS02/opcode_i.html))
- OSDev Wiki, CPU Detection -- the PUSH SP 8086-vs-286+ behavior.
  ([link](https://wiki.osdev.org/User:ChosenOreo/CPU_Detection))

## 5. Building the shipped HDD image with the real FreeDOS 1.3 installer

`disks/build_freedos_hdd.cpp` produces `disks/freedos-hdd.img` by actually
running the genuine, unmodified FreeDOS 1.3 installer against this emulator
end to end -- same technique and same standard as
`ibmpc-at/disks/build_freedos_hdd.cpp` (see `IBM_PCAT_REVIEW.md` §11), never
a hand-crafted filesystem. `disks/hdd_boot_check.cpp` then cold-boots the
finished image standalone. `make hdd-image` and `make hdd-boot-check` tie
both together; neither is part of `check`, because running a real installer
takes real minutes.

### 5.1 Boot path: the floppy, and El Torito is not merely "unverified"

The plan left open whether to boot the installer off the CD via the
Bochs-legacy BIOS's advertised `eltorito` support or off FreeDOS's companion
boot floppy. **The floppy, decisively** -- and the reason is stronger than
risk-aversion.

Reading the ISO's actual El Torito boot catalog (sector 17, `EL TORITO
SPECIFICATION`, pointing at `isolinux/isolinux.bin`) shows the no-emulation
boot image is **ISOLINUX**, whose `isolinux.cfg` chains to **MEMDISK** to
load one of `isolinux/fdinst.img` / `fdlive.img` / `fdx86.img` into extended
memory and present it as a virtual floppy through an INT 13h hook. MEMDISK
reaches extended memory the only ways available -- protected-mode
transitions or the BIOS's own INT 15h AH=87h block move, which `rombios.c`
itself performs in protected mode. This core has no protected mode at all
(§4.1), so that path cannot work here regardless of how good the ATAPI
device is. It is architecturally out of reach this milestone, not a risk to
be managed.

The floppy path is also not a workaround: FreeDOS ships `FD13BOOT.img`
specifically to boot a machine into the installer and read everything else
from the CD drive, and this machine's `configure_factory_cmos()` already
seeds CMOS 0x3D = 0x21 (floppy first, hard disk second). Both files come out
of the one SHA-256-verified zip (`disks/fetch-freedos-cd.sh`).

### 5.2 The real installer flow: one CD, no floppy swapping at all

This is genuinely different from ibmpc-at's flow, and the difference is not
cosmetic. ibmpc-at drives `FD13-FloppyEdition`'s six-disk split archive,
answering "insert diskette containing file A:\FREEDOS.NNN" 114 times. Here
there is **no media swapping whatsoever**:

1. The floppy's `FDCONFIG.SYS` loads `HIMEMX.EXE` and takes `DOS=HIGH`.
2. Its `FDAUTO.BAT` calls `FREEDOS\BIN\CDROM.BAT`, which probes CD drivers in
   a fixed order and settles on the first one present -- **`UDVD2.SYS`**, not
   `ATAPICDD.SYS`. That matters (§5.5). `SHSUCDX` then publishes the drive,
   landing on **D:**.
3. `SETUP.BAT floppy` checks `%CDROM%` and, if `%CDROM%\SETUP.BAT` and
   `%CDROM%\freedos\setup\version.fdi` both exist, `pushd`es to the CD and
   re-invokes `SETUP.BAT cdrom`. **Every stage script, dialog template and
   package then comes off the CD**; the floppy's only job was to boot and
   load the CD driver.
4. `FDSETUP.BAT` runs stages 000-900: language, welcome/proceed, partition
   (`stage400`), reboot, format (`stage500`), package-media discovery
   (`stage600`), the `FDASK000`-`FDASK700` questions (`stage700`), the final
   go/no-go (`stage800`), and completion (`stage900`).

The silent-failure mode in step 3 is worth recording: if the CD is
unreadable, `if not exist %CDROM%\SETUP.BAT` simply falls through to
`:NotFloppy` and the **floppy-resident** installer runs instead. It has all
the stage scripts but no package tree, so it partitions and formats
perfectly and then dies at `stage600` with "Unable to locate the installation
packages." Nothing anywhere says "the CD failed" -- which is exactly how
§5.5's bug presented.

**Dialog mechanics.** Every prompt is a V8Power `vchoice` option box:
arrow keys move a highlight, Enter accepts, and the batch file's `/d N`
switch sets which entry starts highlighted. Read out of the installer's own
scripts rather than guessed, the three destructive prompts -- `stage400`'s
"partition your drive?", `stage500`'s "format your drive?", `stage800`'s
"install now?" -- **all pass `/d 2`, i.e. they deliberately start on "No -
Return to DOS"**. Answering any of them with a bare Enter aborts the install
("The installation of FreeDOS 1.3 has been aborted"), which is what the first
scripted run did. They need Up-then-Enter; every other prompt's default is
already the wanted answer.

**Two timing findings**, both the same class as ibmpc-at's keyboard-flush
"nag" discovery:

- A dialog's *text* appears well before `vchoice` starts reading the
  keyboard -- the batch file is still drawing the frame and option box -- and
  a key pressed inside that window is discarded. Firing on the text's first
  appearance answered nothing. The tool waits for the screen to go quiet
  (~1.2 s of emulated time) first, which is what a person does anyway.
- One press is not enough, and that is not a tuning artifact. `stage300`
  calls `vchoice` from inside a redraw loop (`:LanguagePrompt` re-echoes the
  prompt and re-invokes `vchoice` each iteration), so a single Enter is
  routinely consumed by the wrong invocation and the installer simply sits
  there. The tool therefore *nags*: it re-sends the rule's keys every ~3 s of
  emulated time for as long as the prompt text is still on screen, bounded at
  30 attempts so a genuinely stuck prompt fails the run instead of hammering
  keys forever. This is the same mechanism ibmpc-at needed
  (`IBM_PCAT_REVIEW.md` §11).

  An intermediate version tried to be stricter -- re-press only if the screen
  was byte-for-byte unchanged for ~20 s -- out of a concern that extra presses
  would queue stray keystrokes for the *next* dialog and silently take its
  default ("No", for the three destructive prompts). That version deadlocked
  at the very first prompt and never got past it, so the concern was tested
  rather than assumed: repeated presses are safe here because each rule's key
  sequence is **idempotent against a `vchoice` option box** -- Up clamps at the
  top entry instead of wrapping, so an extra "Up Enter" re-selects the same
  "Yes". In a full run all three destructive prompts took 3-5 presses each and
  every one of them partitioned / formatted / installed rather than aborting.

Matching is also **rule-based rather than position-ordered**, because
`stage400` genuinely reboots the machine and the installer comes back through
the language and welcome screens a second time.

### 5.3 Bug: the factory CMOS never told the BIOS about extended memory

`Machine::configure_factory_cmos()` seeded extended-memory size at CMOS
**0x17/0x18** only. The pinned `rombios.c` (bochs-emu/Bochs@`ff17a0c2`) reads
**0x30/0x31** and nothing else: INT 15h AH=88h ("Get the amount of extended
memory") is `inb_cmos(0x30)`/`inb_cmos(0x31)`, and so is AX=E801's CX, with
memory above 16MB coming from **0x34/0x35** in 64KB units. 0x17/0x18 is never
read at all.

So this 32MB machine reported **zero** extended memory to every query.
HimemX declined to install ("Extended memory is too small or not
available. Driver won't be installed"), `LBACACHE` failed with "XMS error.
43ff Not enough free XMS memory", and -- the load-bearing consequence --
`UDVD2.SYS` failed to load with error #255, because its own documentation
states "Without UHDD, UDVD2 takes 128K of XMS memory for its own input
buffer." No XMS meant no CD-ROM driver at all, so `CDROM.BAT` ended at
"unable to load an appropriate CD/DVD driver".

This was hiding behind the review doc's own hedge: §3's CMOS bytes were
flagged "NOT yet verified against an actual boot", and `chipset.h`'s memory
map asserted extended RAM was "present and counted". It was present, but it
was never counted, because nothing was told where to look.

Fixed by seeding 0x30/0x31 with the same 0x7C00 (31744 KB) already in
0x17/0x18 -- real AT CMOS maps carry the figure in both places, 0x17/0x18 as
the Setup-configured copy and 0x30/0x31 as the POST-verified one, holding
the same value -- and 0x34/0x35 with 0x0100 (256 x 64KB = 16MB above the
16MB line). Neither pair falls inside the 0x10-0x2D checksum range, so the
checksum is unaffected. Regression-tested
(`MachineTest.ConstructorSeedsFactoryCmosConfiguration`, extended to assert
both pairs and that the two copies agree).

### 5.4 Bug: "unreal mode" -- a 32-bit effective address must reach past 64KB

With XMS working, the boot got *further* and then failed harder: the kernel
reported "allocated 46 Diskbuffers = 24472 Bytes in HMA" and then the machine
ran away. A CS:IP hit histogram over the stall window showed the CPU sweeping
the entire 64KB of segment DC86 -- unpopulated upper memory -- i.e. executing
garbage. A per-instruction ring buffer caught the transition exactly:

```
0291:5090  CD 21        INT 21h   (AX=3E34, close file)
DC86:8D01  00 00 ...              <- vectored into nothing
```

Memory was already corrupt by then. The instructions a few hundred steps
earlier said why:

```
0291:045D  2E 0F 01 16 18 00   CS: LGDT [0018]
0291:0463  0F 20 C0            MOV EAX, CR0
0291:0466  40                  INC AX                 ; set PE
0291:0467  0F 22 C0            MOV CR0, EAX
0291:046A  EB 00               JMP short $+2
0291:046D  8E DA / 8E C2       MOV DS,DX / MOV ES,DX   ; DX = 0008h
0291:0471  0F 22 C0            MOV CR0, EAX            ; clear PE
...
0291:043D  33 D2 8E DA 8E C2   XOR DX,DX / MOV DS,DX / MOV ES,DX
0291:0443  66 C1 E9 02         SHR ECX, 2
0291:0447  F3 67 66 A5         REP MOVSD               ; addr32 + opsize32
```

That is **HimemX's XMS block-move routine running in "unreal mode"**: a brief
excursion through protected mode loads a 4GB-limit descriptor into DS/ES,
PE is cleared again, and -- because loading a segment register in real mode
sets its base but leaves the cached limit alone -- the big limit survives, so
a 32-bit `REP MOVSD` with DS=ES=0 can address all of extended memory.

This core no-ops LGDT and MOV CR0 (§4.1, documented), which is fine on its
own. What was not fine is that **both** the 32-bit addressing paths then
truncated the offset into 16 bits:

- `decode_modrm()` did `out.off = uint16_t(ea)`, and
- `string_op()` explicitly ignored the 0x67 prefix, advancing only SI/DI and
  counting in CX.

So every XMS block move wrote to a wrapped, wrong address inside the first
64KB -- corrupting live memory rather than failing -- until the guest
eventually jumped through something the copy had destroyed.

**This falsifies §4.3's premise, and that is the substance of the fix.** That
section chose wrapping partly because "an out-of-range EA in practice means a
decoding bug in this core rather than genuine guest behavior", and dismissed
the unreal-mode case as "unavailable here by construction: expanding a limit
requires a protected-mode descriptor load, and this core has no protected
mode at all". The guest does not care. It performs the descriptor load, this
core ignores it, and the guest proceeds *as if* unreal mode is active --
which, for a core with no descriptors and therefore no limits to enforce, it
effectively is. Letting the full offset reach the bus is the faithful
emulation of the state the guest believes it is in; truncating it is the
silent corruption.

Implemented narrowly, so nothing else changes:

- `cpu80486.h` gains flat `phys32`/`rb32`/`wb32`/`rw32`/`ww32`/`rd32`/`wd32`
  and width-dispatching `mrb`/`mwb`/`mrw`/`mww`/`mrd`/`mwd` that use the
  ordinary 16-bit helpers for any offset a 16-bit form can produce and the
  flat ones only above 0FFFFh. Real-mode wrap-at-the-top behavior for normal
  accesses is therefore bit-for-bit unchanged; the flat forms deliberately do
  not wrap a multi-byte access at 0FFFFh, because with a 4GB limit there is
  nothing there to wrap at.
- `decode_modrm()` passes `ea` through untruncated. The 16-bit branch already
  wraps every intermediate sum into a `uint16_t`, so it can never produce an
  offset above 0FFFFh and is unaffected.
- `string_op()` honors 0x67: ESI/EDI/ECX as pointers and counter, advancing
  the 32-bit registers, while the un-prefixed form still moves only SI/DI and
  decrements only CX, leaving the upper halves alone.

The missing #GP(0) stays a labelled gap. Regression-tested:
`Addr32EffectiveAddressAboveSixtyFourKReachesPastTheSegment` (replacing
`...WrapsIntoTheSegment`, which pinned the old decision),
`Addr32StringOpUsesEsiEdiEcxAndReachesAboveSixtyFourK` (the exact
`F3 67 66 A5` encoding HimemX uses, with both pointers above 64KB and their
low halves differing from the full values so a truncating core fails),
`StringOpWithoutAddr32KeepsUsingTheSixteenBitPointers`, and
`SixteenBitAddressingStillWrapsModSixtyFourK` (strengthened with the address
a non-wrapping core would hit instead).

### 5.5 Bug: READ TOC format 1 was refused, and that hid the whole CD-ROM

With XMS and unreal mode working, `UDVD2.SYS` loaded and `SHSUCDX` published
the drive as D: -- and `dir d:\` still answered **"Error reading from drive
D: data area: drive not ready"**. Every path on the CD was unreachable, which
is why `SETUP.BAT` had been quietly falling back to the floppy installer
(§5.2) and dying at `stage600`.

Tracing every CDB the device received showed the driver issuing exactly three
commands and giving up:

```
43 00 01 00 00 00 00 00 0C 00 00 00  -> CHECK CONDITION, 05h/24h  (INVALID FIELD IN CDB)
00 00 00 00 00 00 00 00 00 00 00 00  -> CHECK CONDITION, 06h/29h  (the reset unit attention, §3.7)
43 00 01 00 00 00 00 00 0C 00 00 00  -> CHECK CONDITION, 05h/24h
```

That is **READ TOC/PMA/ATIP with format 0001b ("Session Information") and a
12-byte allocation length**, and `cmd_read_toc()` implemented only format
0000b, rejecting everything else with INVALID FIELD IN CDB.

The root cause is a stated assumption in §3, not an oversight in the code:
this device's command set was chosen by reading **`ATAPICDD.SYS`**, on the
reasoning that "that driver -- not a spec -- is what will actually probe this
device at boot." It isn't. `CDROM.BAT` tries `AHCICD`, `VIDE-CDD`,
`OAKCDROM`, `GSCDROM`, then **`UDVD2`**, and only reaches `ATAPICDD` fifth
among the bundled drivers; `UDVD2.SYS` is present on the boot floppy, so it
always wins. Verifying against the right consumer is the actual lesson here.

Fixed by implementing format 0001b per MMC: a 4-byte header (TOC data length
= 10, first complete session = 1, last complete session = 1) plus exactly one
TOC track descriptor for the first track of that session (ADR=1/CONTROL=4 for
a data track, track 1, start address 0, honoring the CDB's MSF bit through
the same `put_addr` helper and therefore the same Red Book 150-frame pregap
as format 0). 12 bytes total, which is why a driver asking for this format
sets the allocation length to 0Ch. Formats 2 and up stay refused -- they are
genuinely not implemented, and a driver probing for them is supposed to learn
that. Regression-tested (`ReadTocFormatOneReportsSessionInformation` and
`ReadTocStillRejectsAnUnimplementedFormat`).

With this in place the CD is fully readable: over a thousand ATAPI commands
per session, READ(10)s returning real 2048-byte sectors, and `SETUP.BAT`
handing off to the CD's own installer as designed.

### 5.6 What the installer's own output confirms about the hardware

Worth recording because each line is third-party software independently
agreeing with this machine's configuration:

- `ata0 master: RETROWEB IDE 504MB ATA-0 Hard-Disk ( 504 MBytes)` and
  `ata1 master: RETROWEB CD-ROM 2X ATAPI-4 CD-Rom/DVD-Rom` -- the BIOS
  detecting both devices on separate channels via the genuine ATA/ATAPI
  signature protocol (§3.3).
- `HimemX ... KBC A20 method used` -- the 8042 output-port A20 gate
  (`i8042.h`) driven by real third-party code, and `Kernel: allocated 46
  Diskbuffers = 24472 Bytes in HMA` confirming HMA access above the 1MB line
  actually works.
- `LBAcache ... disk 0x80 CHS=...x16x63 EDD 3.0 0504 MB ISA 01 ATA Master` --
  a third-party cache driver reading back exactly the 16-head/63-sector
  geometry §3.1 chose, through INT 13h.
- `Disk size: 504 Mbytes, FAT16.` from the real `FORMAT.EXE` -- the 504MB
  CHS-ceiling geometry surviving all the way into a filesystem.
- `illegal partition table - drive 00 sector 0` (four times) on the first
  boot: the FreeDOS kernel correctly complaining about a genuinely blank,
  all-zero MBR, which is what a factory-fresh disk has.

### 5.7 Bug: the CD-ROM's access-time model was slower than the drive it models

The first end-to-end attempt reached the package-install stage and then
crawled: each package took ~2.2 billion cycles (**33 seconds of emulated
time**) regardless of size -- `append.zip` is 42,784 bytes, which is 0.14 s of
transfer at this drive's real 307,200 bytes/sec. A full-set install would have
needed ~440 billion cycles, hours of emulated machine time.

Rather than widen the budget, the cost was attributed. First, raw throughput
was measured from a live DOS prompt: `copy d:\packages\base\kernel.zip nul`
(772,512 bytes) took 201,166,713 cycles = **3.05 s**, against 2.51 s of pure
transfer plus a few seeks. **Throughput is correct** -- the drive really is a
2x drive. So the per-package cost was not data transfer.

Instrumenting the pacing accounting over a boot-plus-early-install window gave
the answer directly:

```
reads=2000  seeks=505  charged=140.4s  xfer=14.1s  seek=126.2s
```

**90% of all charged CD time was access penalties**, not data. And the
distribution of how far those 505 "seeks" actually travelled from where the
previous read ended shows why that was wrong:

| distance from previous read's end | count |
| --- | --- |
| < 32 sectors (64KB) | **216** |
| 32-127 | 4 |
| 128-511 | 34 |
| 512-4095 | 80 |
| 4096-32767 | 170 |
| >= 131072 | 1 |

`cmd_read10()` charged the **full published 250 ms average access** to every
read that did not begin exactly where the previous one ended -- including a
read *thirteen sectors behind* the head. No real drive takes 250 ms to re-read
something it just passed, and 43% of the penalties were inside 64KB. This is
the access pattern of DOS running batch files off a CD: the batch file, the
utilities it invokes, and the ISO directory extents are re-read constantly
within a few tens of KB.

Two things were wrong, and both were already contradicted by the file's own
documentation:

1. **The read-ahead buffer did not exist.** §3.2 and `atapi_cdrom.h` both
   asserted that a contiguous read pays nothing "because a real drive's head
   is already there **and its read-ahead buffer holds the data**". The code
   tracked a single LBA position, i.e. a zero-byte buffer. Period 2x drives
   shipped 64KB-256KB buffers.
2. **Access time is distance-dependent, and a datasheet's "average" is a
   one-third-stroke figure**, not the cost of every seek. Charging the average
   flat is the same class of error §4.6 deliberately avoided for the CPU by
   fitting data-dependent instruction costs to both published endpoints
   instead of charging a single number.

Note the direction: this made the emulated drive **slower than the real
hardware it models**. That is still an accuracy bug, and fixing it is not a
speed knob -- the drive's published figures are unchanged and remain the
calibration anchor.

Fixed in `atapi_cdrom.{h,cpp}` with `access_seconds_for()` /
`note_transfer()`, replacing `next_sequential_lba_` with a head position
(`head_lba_`, with a `kHeadUnknown` cold state):

- **Read-ahead buffer**: `kBufferSectors = 32` (64KB, the conservative/slowest
  end of the period range). A request starting inside `[head - 32, head]` pays
  no access time at all.
- **Distance-dependent seek**: `kSeekMinSec + (kAccessSec - kSeekMinSec) * 3 *
  (distance / capacity)`, with `kSeekMinSec = 0.08` (period 2x drives quote
  80-150 ms for a short/single-track access; the low end taken). Calibrated so
  a one-third-stroke seek costs exactly the published `kAccessSec` = 250 ms,
  making a full-stroke seek 590 ms -- in line with period 2x full-stroke
  figures of ~400-600 ms.
- **Cold drive** (post-reset, or media just changed) still pays the full
  published average, because a real drive genuinely has to find its place.
- `cmd_seek10()` uses the same model, so an explicit SEEK costs the real
  distance and leaves the head (and buffer) where a following READ can use it.

The existing realism guard `Read10IsPacedToRealTwoSpeedDriveTiming` -- which
asserts a one-sector read cannot complete faster than real hardware could
deliver it -- still passes unchanged, because it reads on a cold drive.
Regression-tested additionally with
`ReadAheadBufferServesAShortBackwardsReReadWithNoSeek` (a read 13 sectors
behind the head pays no seek) and
`SeekCostGrowsWithDistanceAndStillRespectsTheShortSeekFloor` (a short seek
costs strictly less than a long one, strictly more than the 80 ms floor, and
strictly less than the 250 ms average; a near-full-stroke seek costs more than
the average).

### 5.8 Cost, measured

An earlier draft of this section estimated 60-100 billion cycles for a full
run. That was wrong, and chasing the discrepancy is what found §5.7's pacing
bug -- so the numbers here are measured, not estimated.

The installer's default package answer selects the **full** set
(`fdplfull.lst`, 205 packages), not the base set, because every `FDASK`
question's highlighted default is already the wanted answer and the tool
presses Enter. That is the unmodified installer's own default, so it is what
this machine ships.

Before §5.7's fix, every package cost ~2.2 billion cycles (~33 s of emulated
time) regardless of size, and a run exhausted a 200-billion-cycle budget at
package 150 of 205. After it, the same packages at the same points in the
same flow:

| package | pre-fix | post-fix | ratio |
| --- | --- | --- | --- |
| `util\fdnpkg` | 190.6e9 | 72.0e9 | 2.65x |
| `util\v8power` | 193.4e9 | 73.2e9 | 2.64x |
| `apps\fdimples` | 197.9e9 | 74.4e9 | 2.66x |

A consistent **2.65x reduction in total elapsed cycles**, which is the pacing
model alone -- no other change, same installer, same media, same dialogs.

Measured for a complete run:

- **Boot, partition, self-reboot, format and package discovery**: ~22.4
  billion cycles. (Pre-fix: ~50 billion.)
- **Package install**: ~0.70 billion cycles per package averaged over the 205
  packages, i.e. ~10.6 s of emulated time each.
- **Total to the installer's own completion screen**: 317.5 billion
  cycles, about 80 minutes of emulated machine time at 66 MHz, in
  40 minutes of host wall clock (this emulator sustains roughly
  80-150 million cycles per host second on this hardware, so it runs faster
  than the real machine even though nothing about the machine is sped up).

The run remains dominated by genuinely-paced device work rather than CPU: CD
reads at a real 2x drive's 307,200 bytes/sec with real distance-dependent
access time (§3.2, §5.7), HDD writes at `wd1003`'s rate, and the `vchoice` /
`vdelay` waits the installer itself performs. The tool's default budget is
400 billion cycles, roughly 2.4x the measured requirement, so a slower host or
a future package-set change has headroom before it trips the failure path.

Per CLAUDE.md this is a build-time asset generator, not the shipped emulator,
so none of that is sped up and none of it needs to be -- and §5.7 was fixed
because the drive was *slower* than the hardware it models, not to make the
build faster.

### 5.9 The installed system's own boot menu selects a V86 memory manager

The installer reporting success was necessary but not sufficient, and this is
the gap: `make hdd-image` completed, produced a byte-exact 528,482,304-byte
image with a real MBR and a full 250MB FreeDOS install -- and
`make hdd-boot-check` then failed, with the screen frozen on

```
JemmEx v5.79 [02/02/20]
```

**What is actually happening.** A CS:IP hit histogram over the stall shows the
CPU spinning in a four-instruction loop across `0000:0008`-`0000:0017` -- i.e.
executing the *interrupt vector table* as code. No opcode fired
`on_unimplemented`, so this is not an unrecognized instruction; it is control
flow that went somewhere that does not exist.

That is the signature of a V86 monitor starting up on a CPU that has no V86
mode. JemmEx's own readme describes exactly what it is: it provides EMS/UMB
plus "VCPI services to allow DOS applications running in **V86-mode** to switch
to protected mode" and a "VDS API ... in V86-mode", and devotes a whole section
to "Emulation of privileged Opcodes" -- a V86 monitor is the entire design.
This core is real-address-mode only (§4.1): LGDT/LIDT are documented no-ops and
`MOV CR0` deliberately masks PE off so a read-back still says "real mode". So
JemmEx builds its descriptor tables, writes CR0, jumps into what it believes is
V86 mode, and lands in the IVT.

**This is not a core bug**, and JemmEx cannot detect it: every real 486 has V86,
so it has no reason to check, and §4.1's CR0 masking is a deliberate choice
made so real-mode software asking "am I in protected mode?" gets the true
answer. It is the documented Milestone 1 scope meeting FreeDOS's default
configuration, and it should simply stop being an issue once a later milestone
implements protected mode and V86.

**Why the installer cannot be steered away from it.** This was checked in the
installer's own scripts rather than assumed:

- `fdins900.bat` chooses its CONFIG.SYS template purely from CPU/environment
  detection: `vinfo /m`, then `FEXT=386`, `if errorlevel 4 set FEXT=486`, and so
  on, with separate branches for DosBox/QEMU/VirtualBox/VMware. It then does
  `if not exist %FINSP%\CONFIG.%FEXT% set FEXT=DEF`. There is no `CONFIG.486`
  (nor `CONFIG.EMU`) on the media -- only `CONFIG.DEF`, `.286`, `.186`, `.086`,
  `.DBX`, `.VBX` -- so a 486 always lands on **CONFIG.DEF**.
- `CONFIG.DEF` is the 386+ template, and it **hardcodes** `MENUDEFAULT=2,5`
  rather than using the `$FDEFMENU$` placeholder that `fdins900.bat` substitutes
  for other templates. (`CONFIG.286`, by contrast, defaults to `FDXMS286.SYS`
  and never loads a V86 monitor at all -- but claiming to be a 286 to get it
  would mean lying about the CPU, which is not on the table.)
- No `FDASK` question anywhere asks about memory management: grepping every
  `fdask*.bat` and `en/fdsetup.def` for jemm/ems/xms/memory finds nothing. The
  questions are keyboard layout, target directory, config-file handling,
  package set, boot sector and backups.

So no answer to the unmodified installer produces a JEMM-free configuration.

**The fix, and why it is the smallest honest one.** FreeDOS's own installed menu
already contains the right entry for hardware where the memory manager will not
work -- entry 4, "Load FreeDOS low with some drivers (Safe Mode)", which loads
only `HIMEMX.EXE` (`34?DEVICE=...HIMEMX.EXE`) and no JEMM. HIMEMX is already
proven on this machine: it is what the installer's *own* boot floppy loads, and
it works (§5.3, §5.6).

`SelectRealModeBootMenuEntry()` in `disks/build_freedos_hdd.cpp` therefore
changes exactly one ASCII digit in the FDCONFIG.SYS the installer just wrote:
`MENUDEFAULT=2,5` becomes `MENUDEFAULT=4,5`. Nothing else is touched -- all five
of FreeDOS's menu entries remain present and selectable, the file length is
unchanged so no FAT or directory structure is rewritten, and the only difference
is which entry the menu's own 5-second timeout picks.

Two details worth recording:

- The search is anchored on the whole `MENUDEFAULT=2,5` + `MENU 1 - Load FreeDOS
  with JEMMEX` block, not on the bare string. A full install contains **ten**
  occurrences of `MENUDEFAULT=` in package documentation and exactly **one** of
  the anchored block, so a naive search would have patched a doc file. The tool
  asserts the anchored block is found exactly once and that the digit really is
  `2`, and fails the whole run rather than writing an image if either check
  fails.
- This is a labelled departure under CLAUDE.md's override rules, and it is
  labelled on the machine itself as well as here: a user booting it sees
  FreeDOS's real five-entry menu with "Safe Mode" as the highlighted default and
  can still choose any other entry. It should be reverted when protected mode
  and V86 arrive.

### 5.10 The finished image boots

`make hdd-boot-check` (`disks/hdd_boot_check.cpp`) is the actual bar for done,
rather than trusting the installer's own completion message. It cold-boots the
machine with **nothing but the finished HDD image mounted** -- no floppy, no CD
-- so the BIOS has to fall through drive A: to drive C: via the boot sequence
`configure_factory_cmos()` seeds, and it requires a `C:\>` prompt that has sat
idle (unchanged for ~5 s of emulated time), so a prompt string that merely
flashed past mid-boot cannot satisfy it. It also checks the image is exactly
528,482,304 bytes and carries a `55 AA` boot signature up front, so a bad
image fails loudly instead of after a whole cycle budget of nothing.

Result on the shipped `disks/freedos-hdd.img`:

```
OK: reached an idle C:\> prompt at cycle 1409066356
=== screen ===
  Name           Total           Conventional       Upper Memory
  --------  ----------------   ----------------   ----------------
  SYSTEM      68,336   (67K)     68,336   (67K)          0    (0K)
  HIMEMX       2,192    (2K)      2,192    (2K)          0    (0K)
  COMMAND      3,376    (3K)      3,376    (3K)          0    (0K)
  Free       580,048  (566K)    580,048  (566K)          0    (0K)

Physical hardware networking is not supported at this time.

CD-ROM not configured

Done processing startup files C:\FDCONFIG.SYS and C:\FDAUTO.BAT

Type HELP to get support on commands and navigation.

Welcome to the FreeDOS 1.3 operating system (http://www.freedos.org)

C:\>
```

Everything in that screen is the real software doing what it does on real
hardware, not a stub: `FDCONFIG.SYS` and `FDAUTO.BAT` process, `HIMEMX` loads
and reports its real 2,192-byte footprint, FreeCOM reports genuine
conventional-memory figures for `SYSTEM` (the kernel), `HIMEMX` and `COMMAND`
(itself) with 566K free, "CD-ROM not configured" is correct because this check
deliberately mounts no CD, and the shell's own welcome banner and prompt appear.

The image itself checks out independently: exactly **528,482,304 bytes**
(1024 x 16 x 63 x 512), a `55 AA` signature at offset 510, and an MBR partition
entry of `80 01 01 00 06 0F 3F 00 3F 00 00 00 C1 BF 0F 00` -- bootable flag
`80`, type `06` (FAT16 >32MB), LBA start 63, length `0x000FBFC1` = 1,032,129
sectors, i.e. the whole 504MB disk minus the first track. Read back with
`mtools` it is a genuine FAT16 volume labelled `FREEDOS2022` holding
`KERNEL.SYS`, `COMMAND.COM`, `FDAUTO.BAT`, `FDCONFIG.SYS` and the
`FREEDOS`/`NET`/`GAMES`/`UTIL`/`APPS`/`PGME` trees, with 252MB of its 504MB
still free.

## 6. Milestone 2: protected mode, paging, task switching and the FPU

Milestone 1 stopped at real address mode, and §4.1 enumerated what that
excluded: a specific list of opcodes carried as **documented no-ops**, plus
the four CR0 bits that were accepted and discarded. Milestone 2 makes all of
it real, inside the same two files (`cpu80486.h`/`.cpp`). The concrete bar was
the same shape as Milestone 1's "boots the FreeDOS installer to a live
`C:\>`": a protected-mode program actually running, not a passing unit suite
(§6.9).

The headline result is that both bars now hold at once. `make pm-check` runs a
32-bit protected-mode program through paging, a repaired page fault, a
hardware task switch, a ring-3 excursion and the FPU; and `make
hdd-boot-check` still reaches an idle `C:\>` **at cycle 1409066356** -- the
identical figure §5.10 recorded before any of this existed. A real-mode boot
that is cycle-for-cycle unchanged is the strongest available evidence that
adding protected mode did not perturb real mode.

### 6.1 What became real, and what is still a gap

Every item on §4.1's no-op list now does its published work:
LGDT/LIDT/SGDT/SIDT (real in both modes, since all four are legal in real
mode and HimemX depends on it), LLDT/LTR/SLDT/STR/VERR/VERW/LAR/LSL/ARPL
(protected-mode only), CLTS, LMSW, MOV to/from CR0-CR3, INVLPG, and the whole
x87 ESC range 0xD8-0xDF.

Three §4.1 decisions are now *inverted*, and that is the point rather than a
regression:

- **SMSW no longer reports ET = 0.** CR0.ET is hardwired to 1 on an Intel486
  because the FPU is on-die; §4.1 reported 0 truthfully for an FPU-less core,
  and reporting 0 now would be the lie. `init_state()` sets ET and
  `write_cr0()` refuses to clear it.
- **MOV CR0 no longer masks PE.** It switches mode, both directions.
- **LMSW enters protected mode, and still cannot leave it.** That asymmetry is
  genuine: a 286 could enter protected mode and never exit, and the 386/486
  kept the behavior, so `grp0f01()` re-asserts PE after an LMSW that tries to
  clear it. Leaving needs a MOV to CR0.

Also inverted: the protected-mode-only members of the group are now **#UD in
real mode**, which is what Intel documents ("not recognized in Real Address
Mode") and what §4.1's no-ops were standing in for. `LGDT`/`LIDT`/`SGDT`/
`SIDT`/`SMSW`/`LMSW`/`MOV CRn` deliberately stay legal in real mode, because
FreeDOS 1.3's HimemX executes LGDT plus MOV CR0 in real mode on every XMS
block move (§5.4) and making those fault would break the boot outright.
`SgdtLgdtAndTheControlRegistersStayLegalInRealMode` pins that split down.

Still deliberately absent, and now the complete list:

- **Virtual-8086 mode.** EFLAGS.VM has storage and reads back, but the core
  never enters V86 and none of the mode's machinery exists. This is why
  §5.9's `MENUDEFAULT=4,5` workaround **stays in place**: JemmEx is a V86
  monitor, so protected mode alone does not help it. V86 is the next thing to
  do if the goal is running FreeDOS's default configuration rather than a DOS
  extender.
- **Debug and test registers.** DR0-DR7 round-trip but no breakpoint ever
  fires; TR3-TR7 read 0, because there is no cache model to test and the TLB
  model here is not the silicon's structure.
- **Alignment-check faults (#AC).** EFLAGS.AC and CR0.AM are real storage
  (§4.4) but nothing faults.
- **CPUID**, for the AP-485 reason §4.5 gives, unchanged. (No longer true as
  of §13.1: this core implements CPUID, because the SL-Enhanced IntelDX2
  this machine ships has it and FreeDOS asks.)
- **CR0.NE = 0 does not raise an external interrupt.** With NE clear a real
  486 asserts FERR# and a period board routes it to IRQ13; this chipset has
  no FERR# line, so an unmasked FPU exception is latched in the status word
  and reported only when NE is set (§6.7). Wiring FERR# is chipset work, not
  CPU work.

### 6.2 Segmentation: descriptor caches, and why real mode still has no limit check

Each of the six segment registers gains a `SegDesc` -- the "hidden" half real
silicon loads from the descriptor and keeps using until the selector is loaded
again. That structure, not the selector value, is what every access consults.

**Real mode keeps deriving the base from the selector, and enforces no limit
at all.** This is the §4.3/§5.4 decision preserved deliberately, not an
oversight: `seg_linear()` returns `base + off` with no check when
`!protected_mode()`, so a 32-bit effective address in real mode still reaches
the bus untruncated, which is what makes HimemX's unreal-mode block moves
work. `code32()` and `stack32()` likewise consult `protected_mode()` before
reading the D/B bits, so real mode is unconditionally 16-bit-default no matter
what a previous protected-mode excursion cached. Those two facts together are
why the real-mode boot is cycle-identical.

**The cache surviving a mode change is the mechanism, and it is now modelled
properly rather than accidentally.** `write_cr0()` reloads *nothing* on either
PE transition. That single fact makes both directions work: on the way in, the
instructions between `MOV CR0` and the far JMP run out of the old cached CS;
on the way out, a large limit loaded in protected mode survives, so the next
real-mode segment load sets the base and leaves the limit alone. Milestone 1
got unreal mode right by having no limits to begin with; this core gets it
right by reproducing the actual hardware behavior, and
`ABigLimitLoadedInProtectedModeSurvivesIntoRealModeAsUnrealMode` tests the
mechanism rather than the side effect.

**One deliberate compromise, stated plainly.** The selector fields
(`cs`/`ds`/...) are public, and the embedding chipset and the tests assign
them directly -- Milestone 1's contract. `refresh_real_bases()`, called at the
top of each real-mode `step()`, treats a field that no longer matches the
selector its cache was loaded from as the segment load it is, and re-derives
the base while keeping the limit. Comparing against the recorded selector is
what distinguishes "software wrote DS" from "DS still holds what a
protected-mode descriptor load put there", so a protected-mode *base* as well
as a limit survives into real mode. In protected mode there is no such sweep:
assigning a selector field directly there desynchronizes the cache, and the
header says so.

The full protection rule set is implemented and tested per rule rather than in
aggregate: expand-down limits inverting the check, null selectors legal in
DS/ES/FS/GS but never SS and faulting only on use, SS demanding a writable
segment whose RPL *and* DPL both equal CPL, execute-only code segments
refused at load time, conforming code segments reachable from outside and
keeping the caller's CPL, non-conforming ones demanding an exact match, the
descriptor Accessed bit written on a successful load, and the outward-return
sweep that nulls any segment register the outer level may not keep.

### 6.3 Paging: a correctness-first TLB, and CR0.WP as a genuine 486 addition

`translate()` is the two-level walk: CR3 to page directory to page table, with
U/S and R/W ANDed across the two levels, A set on both entries and D set on
the table entry for a write, CR2 loaded with the faulting linear address, and
a #PF error code carrying the present / write / user bits.

**CR0.WP is implemented because it is a real 486 addition, not a Pentium
one.** With WP clear -- the only behavior a 386 had -- a supervisor write
bypasses a page's R/W bit entirely, which is precisely why copy-on-write was
impossible before WP existed. Both halves are tested
(`SupervisorWriteToAReadOnlyPageSucceedsUntilWriteProtectIsSet`), and a WP
change flushes the TLB.

**The TLB is a 64-entry direct-mapped model, and that choice is labelled.** A
real 486 has a 32-entry 4-way set-associative TLB. What is architecturally
visible to software is not the associativity but the contract: a translation
may be cached, a CR3 write flushes everything, and INVLPG drops one page.
This model honors exactly that contract, and CLAUDE.md's "correctness over
speed" is why it is a simple direct map rather than a structural imitation.
`InvlpgDropsOneTranslationAndACr3WriteDropsThemAll` tests the *stale* case on
purpose -- repointing a page table behind the TLB's back and confirming the
old translation is still used until INVLPG runs -- because that is the
behavior software has to program around, and a core that "helpfully" noticed
the change would be the wrong one.

Paging requires protection: setting CR0.PG with PE clear is a #GP(0), not a
silently dropped bit, and `EnablingPagingWithoutProtectionIsAGeneralProtectionFault`
pins it down.

A 16-, 32- or 64-bit access that straddles a page or segment boundary
translates *every* byte before storing any of them, so a fault cannot leave
half a write behind. That is what makes a #PF restartable, and §6.9's stub
depends on it working: its handler repairs the mapping and IRETDs, and the
faulting instruction runs again and succeeds.

### 6.4 A fault is a non-local exit, and the wasm build needs `-fexceptions`

An x86 fault abandons a partially executed instruction and restarts it. That
is not something a return code models well in an interpreter whose accessors
return values inline in expressions, so `raise()` throws a `cpu80486::Fault`
and `step()` catches it -- the same structure Bochs uses with its
longjmp-based `exception()`. `deliver_fault()` then puts back the instruction's
own EIP, ESP, SS and SS descriptor before vectoring, so the handler sees
exactly what a restart would see; `AFaultRestoresTheStackPointerSoTheInstructionCanRestart`
checks that through the `on_fault` hook, which fires after the restore and
before the handler.

Nested faults escalate the way hardware does: a fault while delivering a fault
becomes #DF, and a fault while delivering *that* is shutdown, modelled as a
halt until reset. `AnUndeliverableFaultEscalatesToDoubleFaultThenShutdown`
covers the whole chain, and the §6.9 stub's harness treats `halted` as
*insufficient* evidence of success for exactly this reason -- a shutdown sets
it too.

**This forced one build change outside the CPU core.** Emscripten disables
exception catching by default, which turns every `throw` into an abort, so
`web/Makefile` gains `-fexceptions`. Without it the first page fault a DOS
extender takes would kill the whole wasm module instead of reaching its
handler. The flag is commented at its one call site with that reasoning. It
costs real code size -- `pc486.wasm` goes from about 102KB to about 189KB, the
exception tables plus this milestone's own new code -- which is the price of
the module surviving a fault at all.

`Cpu::interrupt()` -- the entry point `machine.cpp` calls from its instruction
loop -- also catches. Delivery of a hardware interrupt can itself fault on a
missing or malformed gate, and that has to become a #GP/#DF rather than an
exception escaping into the chipset, which has no idea what a `Fault` is. A
test drove this one out (§6.10).

### 6.5 Bugs found

Two real bugs in this milestone's own new code, both found by evidence rather
than review, and one lesson about the test itself.

**CPL is not the low two bits of CS.** The first `make hdd-boot-check` after
protected mode went in stalled with the screen frozen on

```
HimemX 3.36 [10/29/20] (c) 1995 Till Gerken, 2001-2006 tom ehlert
KBC A20 method used
```

-- i.e. HimemX loaded and then died on its very first unreal-mode excursion.
§5.4 records the routine: `LGDT`, set CR0.PE, then `MOV DS,DX` with DX = 0008h,
running from real-mode CS = **0291h**. `cpl()` had been written as
`protected_mode() ? (cs & 3) : 0`, the identity everyone quotes -- and 0291h's
low two bits are `01`. So setting PE made the core believe it was at ring 1,
and loading a DPL-0 data selector was #GP(8).

The identity is real but conditional: it holds because a *protected-mode* CS
load always sets RPL = CPL, an invariant real mode does not maintain. CPL
actually lives in an internal register written from the code segment's
descriptor on every CS load, and a real-mode CS load sets it to 0 whatever the
segment value happens to be. That is why this trick works on every real 486 --
and it has to, because essentially every period DOS memory manager does it
from an arbitrary CS. The fix is an explicit `cpl_` field written by every CS
load and by nothing else, with the reasoning recorded at `cpl()`.

Worth recording that **the protected-mode stub did not catch this and could
not have**: its real-mode CS is 1000h, whose low two bits are `00`, so CPL was
0 either way. It took the real guest, with the real segment value it happens
to use, to surface it -- the same lesson §5.4 drew.

**The TLB fast path was missing CR0.WP's supervisor exemption.** A unit test
(`SupervisorWriteToAReadOnlyPageSucceedsUntilWriteProtectIsSet`) reported a
#PF with error code 3 on a write that should have gone through. The walk
applies the rule correctly -- a supervisor write to a read-only page succeeds
unless WP is set -- but the TLB-hit path checked only `write && !writable` and
denied it outright. The symptom was as nasty as it sounds: a multi-byte
supervisor write *succeeded on its first byte*, which walks and caches the
entry, and then faulted on its second, which hits the cache. Both paths now
apply the identical rule.

**And the stub itself first "passed" a store it had not actually made.** The
initial `pm_stub_check` run showed the guest reading back the right value from
linear 7MB while the harness's direct peek at physical 7MB found zero. Not a
CPU bug: the **A20 gate is closed at power-on**, so the motherboard was
masking every address to 20 bits, and the store and the read-back were
agreeing with each other at the wrong address. That is correct hardware
behavior -- it is the chipset's job, as `cpu80486.h`'s header says -- and real
software opens the gate through the 8042 before touching extended memory. The
stub now does the same (`mov al,0D1h / out 64h,al / mov al,0DFh / out 60h,al`),
which is both the fix and one more period-accurate step it exercises. The
general lesson is the one worth keeping: a guest-visible read-back is not
proof, because a wrong address is self-consistent. Every paging and
extended-memory check in the stub now also verifies the *physical* frame.

### 6.6 Protected-mode interrupts, gates and task switching

The IDT holds gates, not vectors, and all five gate types work: 16- and 32-bit
interrupt gates (which clear IF) and trap gates (which do not), plus task
gates. An interrupt gate's DPL is checked against CPL for a software `INT n`
and deliberately **not** for a hardware interrupt or an exception -- the
exemption exists so a user program cannot simulate a device interrupt, and
`SoftwareIntChecksTheGateDplButAHardwareInterruptDoesNot` tests both halves
against the same vector.

Inter-privilege entry takes SS and ESP from the TSS slot for the target ring
and pushes a five-dword frame (EIP, CS, EFLAGS, ESP, SS) so IRETD can restore
the interrupted stack; the error code, where the exception has one, is pushed
last. Call gates do the same for a CALL, including copying the gate's declared
parameter count from the old stack to the new one, and a JMP through a call
gate is refused if it would change privilege -- only a CALL leaves a way back.

Task switching is a real hardware task switch: the outgoing register file is
written into the outgoing TSS, busy bits and the back-link are maintained
(a JMP hands over and clears the outgoing busy bit; a CALL or a task-gate
interrupt nests, sets the back-link and EFLAGS.NT; IRETD with NT set returns
through the back-link), CR3 is reloaded per task, CR0.TS is set so the first
FPU instruction in the new task traps #NM, and LDTR is loaded before the
segment registers because their selectors may be LDT selectors. Both 32-bit
(104-byte) and 16-bit (44-byte) TSS layouts are handled.

I/O privilege is real too: IN/OUT/INS/OUTS and CLI/STI are permitted at
CPL <= IOPL, and above that the TSS's I/O permission bitmap decides one port
at a time, where a *set* bit denies. HLT and the control registers are ring 0
only. POPF and IRET silently decline to change IOPL outside ring 0, or IF above
IOPL, rather than faulting -- which is the documented behavior and an easy
thing to get wrong in the direction of a spurious #GP.

### 6.7 The FPU: 80-bit storage, and the one labelled precision departure

The register file holds the real 80-bit extended format (`Float80`: a 64-bit
significand with an explicit integer bit, plus a sign/exponent word), not a
host float. That is the load-bearing choice: `FLD tbyte` / `FSTP tbyte`
round-trip bit-exactly for denormals, infinities and NaN *payloads*, and two
tests use patterns (a QNaN carrying `C123456789ABCDEF`, a denormal with only
the lowest significand bit set) that no host `double` could carry through.
FCHS and FABS are bit operations on the sign field for the same reason, so
they work on a NaN exactly as hardware does.

The status, control and tag words are all real: TOP in status bits 11-13 (so
FNSTSW is how software reads the stack pointer at all), the six exception
flags with their masks, SF qualifying IE for a stack fault with C1
distinguishing overflow from underflow, C3/C2/C0 as a three-way compare
result and as FXAM's class code, and the precision- and rounding-control
fields.

**The one departure, stated rather than implied.** Arithmetic converts to the
host's `long double` and back. On x86-64 that *is* the 80-bit format and the
result is bit-exact; on arm64 and in a wasm build `long double` is a wider
IEEE binary128, so computing there and storing to 80 bits double-rounds, which
can differ from hardware's single rounding in the last significand bit for a
small fraction of operands. Two things narrow it: precision control is applied
explicitly on every result, so a program that selects single or double
precision gets exactly the width it asked for (tested), and rounding control
is honored explicitly where software actually depends on it -- the integer
conversions FIST/FISTP/FRNDINT, which is what a C compiler's `(int)` cast
drives. Arithmetic always rounds to nearest, the reset default; that last part
is a documented gap, not a claim.

Three more genuine details worth keeping:

- **The DC and DE encodings reverse the subtract and divide forms.** With
  ST(i) as the destination, `DC E0+i` is FSUBR and `DC E8+i` is FSUB -- the
  opposite way round from the D8 forms. Intel documents it, every assembler
  special-cases it, and `TheDcAndDeEncodingsReverseTheSubtractAndDivideForms`
  pins both directions down.
- **The exception report is deferred.** An unmasked exception does not trap
  the instruction that caused it; it surfaces on the next *waiting* FPU
  instruction. That is exactly why the seven "no-wait" encodings (FNCLEX,
  FNINIT, FNSTSW, FNSTCW, FNSTENV, FNSAVE) are specified not to check -- a
  handler has to be able to inspect and clear the very error it was called
  for. Implemented and tested as a sequence.
- **FENI, FDISI and FSETPM are genuine no-ops on this part**, not gaps: 8087
  interrupt controls and the 287's protected-mode hint, which the 387 and 486
  ignore.

CR0.EM and CR0.TS both route ESC opcodes to #NM, and CR0.MP gates WAIT on TS
separately -- MP exists precisely so the two can be trapped independently, and
the test checks that TS alone leaves WAIT alone.

### 6.8 Cycle costs for the new instructions

Same discipline as §4.6, with the same distinction between a cited number and
a labelled model, and the same reason for caring: the FPU cost spread is an
order of magnitude (FDIV 73 against FADD 8, FSQRT 83), so a flat per-ESC
figure would make period floating-point benchmarks read as wrongly as the
undercosted integer MUL/DIV did on ibmpc-at.

Directly charged published figures: the descriptor-table and task-management
instructions keep the costs §4.6 already carried; the protected-mode rows of
the far control transfers (JMP far 19, CALL far 20, RET far 13 within a
privilege level and 17 across one, IRET 36) against their real-mode rows (17,
18, 13, 15); and the x87 rows.

Explicitly labelled as this core's readings rather than citations:

- **Where an x87 row is a range, the floor is charged and the
  data-dependence is not modelled.** This is a deliberate departure from
  §4.6's four fitted models: there, Intel documents the *mechanism* behind
  each range (early-out multiply, bit-at-a-time scan) so a curve can be
  fitted to both endpoints. For the x87 ranges no such mechanism is
  published, and inventing a curve would be less honest than charging the
  published minimum and saying so.
- **The integer-operand arithmetic forms** (FIADD/FISUB/FIMUL/FIDIV/FICOM)
  are charged as the operation's published figure plus FILD's -- a stated
  composition of two published numbers, not a citation of the combined row.
- **Call gates and task switches** get one figure each (35/32 for a gate,
  199 for a switch) covering every sub-case the hardware distinguishes.
- **Fault delivery** is 26 in real mode (INT3's figure, the same one
  `interrupt()` uses) and 44 in protected mode (the published same-privilege
  gate row); the higher inter-privilege figure is not separated.

One cost bug was fixed while writing the timing tests: the arithmetic ESC
opcodes were charging a memory-load cost *on top of* the published figure,
but the table gives one number for "FADD ST(i),ST / m32real / m64real" -- the
load is already inside it, exactly as it is for the integer MUL where memory
and register operands cost the same.

### 6.9 The end-to-end proof

`pm_stub_check.cpp` (`make pm-check`) is Milestone 2's "does this actually
work?", and it is deliberately a *program* rather than a test fixture: it
assembles, byte by byte, the smallest thing that does what a DOS4GW-class
extender does at startup, and runs it against the real `pc486::Chipset` -- the
same 32MB memory map and bus the browser build uses. Each step leaves an
observable value in a results block, and `main()` checks all of them, so every
value asserted is one the *guest program* computed.

What it exercises, in order: the canonical real-mode entry sequence (open the
A20 gate through the 8042, LGDT, LIDT, set CR0.PE, far-JMP into a 32-bit flat
code segment); 32-bit arithmetic and a store to linear 7MB, which no real-mode
addressing form can reach; the FPU on exactly representable values (2.5 * 4.0,
sqrt(16), an integer conversion); paging, writing through a *virtual alias* so
the value only lands correctly if the walk is real; a deliberate page fault
whose handler reads CR2 and the error code, installs a page table, INVLPGs and
IRETDs, so the faulting instruction restarts and succeeds; LTR and a far CALL
to a TSS, with the second task running on its own stack, clobbering EAX and
IRETDing back through the back-link; a hand-built IRETD frame returning
outward to ring 3, a DPL-3 interrupt gate switching to the ring-0 stack out of
the TSS and back out again; and finally clearing CR0.PG, dropping to a 16-bit
code segment, clearing CR0.PE and far-JMPing back to a real-mode CS.

Two harness details that matter. `halted` is **not** accepted as evidence of
success, because an undeliverable fault escalates to #DF and then to shutdown,
which sets it too (§6.4) -- the run must also take exactly one fault, the
deliberate #PF. And every paging and extended-memory result is checked against
the *physical* frame as well as read back through the guest, for the A20
reason in §6.5.

It exits non-zero on any failed check, so it is a regression test as well as a
demonstration, and it is fast (no images, no BIOS) -- so unlike
`hdd-boot-check` it is part of `make check`.

Current result: 24 of 24 checks pass.

### 6.10 Tests

The suite went from 290 cases to **389**. `tests/cpu80486_test.cpp` grew from
80 semantic cases to 174 across four fixtures, and
`tests/cpu80486_timing_test.cpp` from 49 to 56.

- **`Cpu80486Test`** (88, was 80): the Milestone 1 coverage §4.7 describes,
  plus the eight cases that replace the ones asserting the old no-op
  behavior -- LGDT/LIDT actually loading the registers (including the 16-bit
  form's 24-bit base truncation against the 32-bit form's full four bytes),
  SGDT/SIDT storing them back, SMSW reporting hardwired ET, LMSW's
  enter-but-never-leave asymmetry, MOV CR0 both directions, PG-without-PE
  faulting, CLTS, and the protected-mode-only opcodes taking #UD in real mode
  while the real-mode-legal half stays legal.
- **`Cpu80486PmTest`** (47): a fixture that enters protected mode by
  *executing the real sequence* rather than poking state, so everything it
  observes is reached the way software reaches it. Covers CS.D driving the
  operand-size default and the prefix toggling it, limit and access-rights
  faults one rule at a time, null selectors, far transfers including
  conforming segments and call gates, the LDT, VERR/VERW/LAR/LSL reporting
  through ZF instead of faulting, ARPL, every gate type, inter-privilege
  entry and IRETD's outward return, the #DF escalation chain, LTR and both
  task-switch flavors, the I/O permission bitmap, and the return to real mode
  with unreal mode intact.
- **`Cpu80486PagingTest`** (7): translation through a virtual alias, #PF error
  codes discriminated bit by bit, CR0.WP's supervisor exemption both ways, A
  and D bits, the stale-TLB contract with INVLPG, and a user access to a
  supervisor page.
- **`Cpu80486FpuTest`** (32): the 80-bit round-trips, all three real and all
  three integer widths, exact arithmetic including the reversed DC/DE forms,
  precision and rounding control, the compare and FXAM condition codes,
  ordered vs unordered compares, stack faults in both directions, the control
  and status words through memory and AX, FSAVE/FRSTOR and FSTENV, packed
  decimal, the transcendentals on exact values, #NM from EM and TS, WAIT
  gated by MP, and the deferred exception report as a sequence.
- **`Cpu80486PmTimingTest`** (4): the protected-mode-only opcode costs, the
  protected-mode rows of the far control transfers, the task-switch figure,
  and protected-mode fault delivery -- all of which need a fixture that is
  actually in protected mode.

Writing the suites found the two core bugs in §6.5 plus the `interrupt()`
exception leak in §6.4, and four bugs in the tests themselves worth noting
because three are traps anyone would fall into again: `D8 /2` is FCOM
**m32real** where `DC /2` is the m64real form, so pointing D8 at a double
silently compares the low half as a float; an inter-privilege call gate needs
LTR to have run, or it is a #TS rather than a transfer; an outward IRETD nulls
the data segment registers, so ring-3 code has to reload DS before it can
address anything; and one test indexed past the end of the fixture's 1MB
array, which is undefined behavior that read 0 until unrelated edits made it
read 2 -- replaced with an in-bounds aliasing check that proves the same
thing more directly.

### 6.11 Sources

- Intel 80486 Programmer's Reference Manual (1990/1992) -- protection,
  segmentation and descriptor formats, paging and the page-table entry bits,
  interrupt and exception delivery, task switching and the TSS layouts, the
  I/O permission bitmap, CR0 including WP and the FPU control bits, and the
  floating-point chapters (control/status/tag words, per-instruction
  behavior). The specific section is named inline at each rule.
- Intel AP-485, *Intel Processor Identification and the CPUID Instruction* --
  unchanged from §4.8. (Its processor list is also what §13.1 reads the
  other way: the SL-Enhanced IntelDX2 has CPUID, and this core now
  implements it.)
- Quantasm, *80x86 Integer Instruction Set (8088 - Pentium)* -- the 486
  column, including its separate protected-mode rows for the far control
  transfers and its x87 rows.
  ([mirror](https://www2.math.uni-wuppertal.de/~fpf/Uebungen/GdR-SS02/opcode_i.html))
- Bochs -- cited for *structure*, not behavior: its longjmp-based
  `BX_CPU_C::exception()` is the precedent for modelling a fault as a
  non-local exit out of a partially executed instruction (§6.4).

## 7. Milestone 3: VGA mode 13h, the DAC, and the VESA BIOS Extensions

Milestones 1 and 2 gave this machine a CPU that can run a DOS game's code.
This milestone gives that code a screen to draw on: the 256-colour modes
period software actually uses, the real palette hardware behind them, and
the VESA interface a program asks for them through.

The bar was the same shape as §5's "boots the FreeDOS installer to a live
`C:\>`" and §6.9's protected-mode stub: not a passing unit suite, but a
program the machine actually boots, whose pixels are then checked one by
one. `make vbe-check` is that proof, and it passes with **zero failures**
against the real, unmodified BIOS and video BIOS.

The class is still called `Ega` for continuity with Milestone 1, but its
own header comment always said it represents the VGA card; §7.1-§7.3 are
the parts of that card Milestone 1 stopped short of.

### 7.1 Chain 4, and why mode 13h "looks linear" on planar hardware

Software sees mode 13h as a flat 64,000-byte frame buffer at `A000:0000`,
one byte per pixel. The hardware underneath is the same four-plane VRAM
every other EGA/VGA mode uses. Chain 4 (Sequencer Memory Mode, SR04 bit 3)
is what reconciles the two: it makes CPU address bits 0-1 the plane select
and drops them from the per-plane offset, so four consecutive CPU bytes are
four *different* planes at one offset, and every plane contributes one
pixel per memory cycle instead of one bit.

This file's VRAM was already stored the way the real part is wired --
byte-interleaved, `vram[(plane_offset << 2) + plane]` (ibmpc-at's
IBM_PCAT_REVIEW.md §12, where that engine was built). Chain-4's
decode against that layout collapses to the identity:

```
((off >> 2) << 2) + (off & 3)  ==  off
```

so `mem_read`/`mem_write` in a chain-4 mode reach `vram[offset]` exactly.
That is not a shortcut taken to save work -- it is the reason mode 13h
looks linear on planar silicon, and it falls out of the layout rather than
being asserted on top of it.

Two details are easy to get wrong and are tested directly:

- **Map Mask still applies.** Chain 4 changes *which* plane an address
  reaches; it does not bypass the Sequencer's plane-enable wires. A mode-13h
  driver that narrows Map Mask genuinely stops some pixels landing, which is
  the same silicon "mode X" planar tricks are built on.
- **Chain 4 outranks odd/even chaining.** Both bits set is a nonsense
  combination software can still program, and the real part resolves it in
  favour of chain-4's 2-bit plane select.

### 7.2 The DAC (0x3C6-0x3C9), and the one number that matters

256 colour registers, three channels each, **six** significant bits per
channel -- the top two are not wired, which is why 8-bit-minded code gets a
washed-out picture on real hardware rather than an error. Implemented as
the real part behaves, not as a convenience table:

- Separate **read and write index registers**, each with its own R→G→B
  sub-counter that auto-advances to the next entry on every third access.
  That is how period code loads a whole palette with one `OUT` to 0x3C8 and
  768 more to 0x3C9, and keeping the two sides independent means a driver
  reading one entry part-way through writing another corrupts neither.
- The **DAC State register** (0x3C7 read) reports which side was addressed
  last: 3 = read mode, 0 = write mode.
- The **PEL Mask** (0x3C6) is ANDed with the pixel value *between the shift
  registers and the DAC's address lines* -- it does not alter a single
  stored colour, which is exactly why period code uses it for fades and
  16-colour-in-256-mode tricks. The renderer applies it there too, rather
  than merely storing it.

Scaling 6 bits to the renderer's 8 uses `(v << 2) | (v >> 4)`, which maps
0→0 and 63→255 exactly. A real DAC drives a full-scale analog ramp, so 63
*is* maximum brightness; the plain `v << 2` some emulators use tops out at
252 and quietly darkens every white on screen. This is the same full-scale
convention `DecodeEgaColor` already used for its 2-bit channels.

### 7.3 Resolution comes from the registers, not from the mode number

The renderer never learns it is "in mode 13h". It reads the same registers
a real CRT controller reads, exactly as ibmpc-at's IBM_PCAT_REVIEW.md §16
established for mode 10h. The
values below are not guesses about what mode 13h ought to program -- they
are what this machine's own BIOS was observed to write, read back off the
live card by `vbe_mode13_check`:

```
CRTC R01 Horizontal Display End = 79
CRTC R12 Vertical Display End   = 399
CRTC R09 Maximum Scan Line      = 1 (scan doubling off)
CRTC R13 Offset                 = 40 -> 320 bytes/scan line (4-byte address unit)
GC   R05 Shift Register field   = 2 (2 = 256-color)
AC   R10 bit 6 (8-bit color)    = 1
```

Read naively that is a **640x400** screen with an **80-byte** scan line --
every one of those three numbers is wrong, and each is corrected by a
different real register:

- **640 → 320.** In a 256-colour mode the VGA spends two dot clocks per
  pixel. The Attribute Controller's "8-bit colour" bit (AR10 bit 6) is how
  the hardware knows, and how `attr_8bit_color()` halves the width. The CRTC
  genuinely is programmed for 640 dots of horizontal timing.
- **400 → 200.** Maximum Scan Line = 1 means two scan lines per row, so the
  400-line raster carries 200 rows. This is a *different* mechanism from
  R09 bit 7's scan doubling, which mode 13h leaves off; both exist and the
  renderer honours both.
- **80 → 320.** The Offset Register is not a byte count. The CRTC's address
  counter is scaled by two register bits -- Underline Location (R14) bit 6
  selects **doubleword** mode (4 bytes/unit) and Mode Control (R17) bit 6
  selects **byte** mode (1 byte/unit), with word mode (2) the default.
  Mode 13h sets doubleword, so its Offset of 40 means 40 × 2 × 4 = 320
  bytes. Mode 10h's *identical* Offset of 40 means 40 × 2 × 1 = 80 in byte
  mode. Hardcoding either figure breaks the other mode; `crtc_address_unit_bytes()`
  derives it, and `start_byte_offset()` scales the Start Address the same
  way so a hardware scroll moves by whole scan lines.

`DetectScreenMode` branches on the Graphics Controller Mode register's
Shift Register field being 2 -- the real CRT controller's own 256-colour
selector -- not on the Sequencer's Chain-4 bit, which is an addressing
choice software can make independently of how pixels are shifted out. Field
value 3 is not a mode real VGA silicon defines and still renders as an
honest black frame.

### 7.4 VESA: the card's extension registers, and a labelled substitution

VBE lives in the video card's own ROM. This machine's card ROM is the
freely-licensed VGA BIOS substituted for IBM's still-copyrighted one
(§6, `roms/fetch-bios.sh`), and like every real SVGA card's ROM it
implements VBE by driving a set of **vendor-specific extension registers**
that only it knows about -- an index port and a data port exposing a bank
of 16-bit registers. Real cards all worked this way (Tseng's ET4000
extended CRTC set, Cirrus's, S3's).

**This is a departure and is labelled as one**, in `ega.h` and here. The
*shape* is period-correct; the specific register numbers are not any real
1993 card's. They are the interface this substitute firmware expects, and
they exist here only because that firmware is this card's ROM. The card is
a compatible stand-in, not a clone of a named part -- the same rule
CLAUDE.md applies to the BIOS and to FreeDOS.

Without those registers the ROM's entire `AH=4Fh` handler is inert: it
gates on the card answering the probe, and the baseline run before any of
this existed returned from all four calls with **AX unchanged** (0x4F00,
0x4F01, 0x4F02, 0x4F03), video memory at 0xA0000 reading 0xFF, and the
legacy `AH=0Fh` cross-check still reporting text mode 3.

Two register semantics carry real weight:

- **The ID register is a probe, not a version field.** Writes stick only
  for revisions this card implements; an unknown revision must *not* stick,
  or the probe wrongly concludes the card speaks it.
- **GETCAPS is a query mode.** See §7.5.

`VideoMemory` reports the card's genuine 256KB and is read-only -- software
cannot solder on more RAM.

### 7.5 Bug found: GETCAPS, and an empty VESA mode list

The first working implementation answered 4F00, 4F02 and 4F03 correctly and
displayed mode 13h correctly -- and reported **zero** VESA modes. 4F01
failed for every mode number.

The cause was a register semantic I had ignored. The Enable register's
GETCAPS bit (0x02) is not a display mode; while it is set, reading the
XRES/YRES/BPP registers reports the card's **maxima** rather than the
current mode's values. The card's ROM uses exactly that to decide which
modes to advertise, in `mode_info_check_mode`:

```
  call dispi_get_max_xres
  cmp  [si+2], ax
  ja   vbe_mode_unsup
  ...
```

and `dispi_get_max_xres` sets GETCAPS, reads XRES, and restores Enable.
Without the bit implemented, every candidate mode was compared against the
*current* geometry -- zero on a card no one had programmed yet -- so every
mode was rejected and the list came out empty.

Worth recording is how nearly this hid: raising the reported VRAM from
256KB to 1MB changed nothing, which looked like proof the filter was not
about memory and initially pointed at a missing PCI linear-framebuffer
probe (the ROM does build as the PCI variant). Reading the firmware's own
source at the pinned revision, rather than reasoning from the symptom,
found the actual gate in four lines.

With GETCAPS implemented the card advertises the three modes it can truly
show, all 8bpp, all inside 256KB: **100h** (640x400), **150h** (320x200)
and **151h** (320x240). 640x480x256 needs 307,200 bytes and is correctly
*not* offered.

### 7.6 The one thing that does not work, and why it is not a bug here

`4F01` for mode **13h** returns `AX=0x014F` -- function supported, call
failed. That is the firmware, not this card. Its `mode_info_find_mode`
searches `vbe_mode_list`, which holds VESA-defined mode numbers (≥100h)
only; 13h is a VGA mode number and is simply not in the table. The VBE spec
defines function 01h over its own mode numbers, so declining is within
spec, and there is nothing the hardware side can do about it short of
patching a third-party ROM.

Mode 13h is still fully reachable over VESA: `4F02` with `BX=13h` sets it
(the ROM routes a sub-100h mode number to the legacy mode-set path), and
`4F03` reports it back. The harness asserts the `0x014F` answer explicitly
rather than skipping it, so a future firmware bump that starts answering
cannot slip by unnoticed.

### 7.7 SVGA linear modes

Because the card advertises 100h/150h/151h, it has to be able to show them
-- advertising a mode the screen cannot display would be exactly the kind
of quiet lie this codebase avoids. In an SVGA mode the planar engine is out
of the picture entirely: no latches, no Map Mask, no Graphics Controller
ALU. The 64KB aperture at 0xA0000 is a plain window onto a linear
byte-per-pixel frame buffer, slid by the Bank register, and geometry comes
from the extension registers rather than the CRTC (the ROM leaves the CRTC
holding whatever the previous mode left, so reading it there would read
stale values). VirtWidth and the X/Y offset registers pan a wider logical
line, the SVGA equivalent of the CRTC Start Address and how period software
double-buffers.

Leaving an SVGA mode has to hand the memory window back to the planar
engine intact, or every subsequent text/EGA mode reads the wrong bytes;
that round trip is tested directly.

### 7.8 The end-to-end proof

`vbe_mode13_check.cpp` assembles a **445-byte** 16-bit program, writes it
into the first sector of a 1.44MB floppy image with the `0xAA55` signature,
and lets the real BIOS boot it off the real floppy controller -- an
ordinary DOS boot disk, nothing injected. Nothing in the results is
asserted by the emulator about itself; every value is what the guest
program computed or what the shared renderer produced.

The guest runs in two phases, parking on a memory poll between them so the
SVGA screen can be captured before the mode-13h set wipes it:

1. `4F00` with a "VBE2"-tagged buffer; `4F01` for 13h and for 100h; `4F02`
   into 100h; `4F03`; palette entries 64-66; a 640x400 pattern; a read-back
   through the window.
2. `4F02` into 13h; `4F03`; legacy `AH=0Fh` cross-check; palette entries
   32-37 loaded through 0x3C8/0x3C9 and entry 32 read straight back through
   0x3C7/0x3C9; a 320x200 pattern; two pixels read back.

Both patterns are corners, a centre point, and a ten-pixel horizontal run,
plus assertions that the pixels immediately around the run are background.
That combination is deliberate: an off-by-one scan-line stride, a wrong
chain-4 decode, or a mis-scaled DAC channel each fail at least one of them
rather than looking close enough.

Results, at cycle 223,000,174 (about 1.4s of host time):

```
  4F00 Return Controller Information -> AX       79           ok
      signature="VESA" version=2.0 total memory=256 KB
      OemStringPtr -> "Bochs VBE (C) 2002-2026 ..."
      VideoModePtr -> 3 mode(s): 100h 150h 151h
  4F01 (mode 13h, a VGA mode number) -> AX       335          ok   (0x014F, see 7.6)
  4F01 Return Mode Information (mode 100h) -> AX 79           ok
      100h: XRes=640 YRes=400 bpp=8 planes=1 model=4 bytes/line=640 winA=A000h
  4F02 Set VBE Mode (BX=13h) -> AX               79           ok
  4F03 current mode -> BX                        19           ok
  legacy INT 10h AH=0Fh cross-check -> AL        19           ok
  DAC State register reads "read mode"           3            ok
  entry 32 red/green/blue read back via 0x3C9    63/21/0      ok
  guest read back A000:0000                      32           ok

  --- rendered mode-13h screen (shared ega_render.cpp) ---
  rendered width / height                        320 / 200    ok
  pixel (0,0)     = palette 32   RGB(255, 85,  0) ok
  pixel (319,0)   = palette 33   RGB(  0,255,  0) ok
  pixel (0,199)   = palette 34   RGB(  0,  0,255) ok
  pixel (319,199) = palette 35   RGB(255,255,255) ok
  pixel (160,100) = palette 36   RGB(130, 65, 32) ok
  ... 10-pixel run on row 50, and its four background neighbours ... ok

ALL CHECKS PASSED (0 failures)
```

`RGB(255,85,0)` from DAC entry 32 = (63,21,0)/63 is the §7.2 full-scale
scaling doing its job end to end: 63→255 and 21→85, through the real port
interface, the real chain-4 memory path and the shared renderer.

The SVGA phase passes identically at 640x400 with palette entries 64-66.

`make check` runs this alongside `pm-check` and the unit suite, so a
regression fails CI rather than being noticed on screen later.

### 7.9 Regression evidence

Mode 13h and the DAC touch the memory path and the port map that every
other mode depends on, so "did this break Milestone 1?" needed answering
directly rather than by inspection:

- **411/411 unit tests pass**, the 389 from Milestone 2 plus 22 new. No
  test was removed.
- **`make hdd-boot-check` still reaches a live `C:\>`** on the shipped
  FreeDOS image, with HimemX loaded and 566K free -- the full real-mode
  text-mode boot path, unchanged.
- **`make pm-check` still passes**, unchanged.

Two existing assertions in `ega_render_test.cpp` *did* change, deliberately:
they asserted that Shift Register field 2 produced `kUnsupportedGraphics`
and a black placeholder frame, which was Milestone 1's honest statement
that it did not implement 256-colour modes. Field 2 now renders, and the
placeholder assertion moved to field 3 -- a value real VGA silicon genuinely
does not define.

The browser front end needed no change at all: it already sized its canvas
from `renderWidth()`/`renderHeight()` each frame, so 320x200 and 640x400
arrive correctly without a special case. Only its comment, which listed the
modes, was updated. There is deliberately no Playwright test for this: the
guest program needs ~223M CPU cycles to reach its pixels, and at this
machine's genuine 66MHz that is real seconds of wall clock that a browser
test would have to sit through at real speed (CLAUDE.md forbids speeding the
CPU up outside a test-only flag this machine does not define). The native
harness is the right home for it.

### 7.10 Tests

- **`ega_test.cpp`** (+15): the DAC's auto-advancing write path and
  independent read index, the 6-bit truncation, the DAC State register, the
  PEL mask's default and round-trip; chain-4's four-bytes-to-four-planes
  decode in both directions, Map Mask still gating it, and its priority over
  odd/even; the extension registers' 16-bit-at-one-address access, the ID
  probe refusing an unimplemented revision, GETCAPS reporting maxima without
  losing the programmed values, the read-only VideoMemory register, the
  clear-on-enable and NoClearMem behaviours, the bank-slid linear window
  (including reading 0xFF past the card's real 256KB), the round trip back
  to planar decoding, and port ownership.
- **`ega_render_test.cpp`** (+7): mode 13h's resolution derived from the
  CRTC *and* the Attribute Controller (proved by clearing the 8-bit-colour
  bit and watching the width become 640), byte-per-pixel decode through the
  live DAC, the doubleword address unit changing the scan-line stride, the
  PEL mask acting between pixel and DAC, the Start Address scrolling by
  whole address units, and the SVGA path's geometry and panning.

Every rendered-pixel assertion is exact. A DAC channel scaling wrong by one
step is a real bug, not a rounding preference.

### 7.11 Sources

- **IBM VGA hardware documentation** -- the Sequencer Memory Mode Chain-4
  bit, the DAC's 6-bit channels, PEL mask and index/state registers, the
  Attribute Controller Mode Control 8-bit-colour bit, and the CRTC address
  scaling from Underline Location bit 6 (doubleword) and Mode Control bit 6
  (byte). Each rule is named inline at its use site.
- **VESA BIOS Extensions (VBE) 2.0** -- functions 00h-03h, the VbeInfoBlock
  and ModeInfoBlock layouts, the "VBE2" request tag, and the VGA-vs-VESA
  mode-number split behind §7.6.
- **The card's own ROM source**, `vgabios/vbe.c` at the revision this
  machine's pinned `VGABIOS-lgpl-latest.bin` is built from
  (`bochs-emu/VGABIOS`) -- cited for what the *firmware* requires of the
  hardware, not as a behavioural model: `mode_info_check_mode` and
  `dispi_get_max_xres/yres/bpp` (§7.5), and `mode_info_find_mode` /
  `vbe_mode_list` (§7.6).
- Bochs `vgacore.cc` -- unchanged from ibmpc-at's IBM_PCAT_REVIEW.md §12,
  still the cross-check for the
  planar memory engine that chain-4 sits alongside.

## 8. The browser build could not keep up with its own clock

Milestones 1-3 were all verified natively: 411 GoogleTest cases, plus
`pm-check`, `vbe-check` and `hdd-boot-check` as end-to-end proofs. All of
them still pass. None of them measures the one thing that turned out to be
broken in the shipped product: whether the **browser** build can run this
machine at 66 MHz in real time without starving the page of input.

It could not. Reported symptom: keystrokes taking seconds to register or
vanishing entirely, with the tab pegged near 100% CPU.

### 8.1 The fact: the interpreter was slower than the machine it emulates

Measured in headless Chromium, at an idle FreeDOS `C:\>` prompt, by calling
`machine.runCycles(20_000_000)` directly and timing it:

```
338 ms per 20M cycles  ->  59.2 M cycles/sec
```

The machine's own clock is **66.0 M cycles/sec**. The emulator ran at 90%
of the speed of the thing it was emulating, so it could never catch up.

That single fact explains the whole reported symptom, and it is worth
spelling out why the failure mode is *freezing* rather than just *running
slow*. `app.js`'s frame loop asked for `dtSeconds * 66e6` cycles once per
animation frame, with `dtSeconds` clamped to 0.25 s, and ran them in one
synchronous `runCycles()` call. Once the machine fell behind, it settled
into a stable, self-sustaining loop:

- a frame asks for the clamped maximum, 16.5M cycles,
- at 59 M/sec that call takes ~280 ms, during which the main thread
  dispatches **nothing** -- no `keydown`, no `click`, not even the next
  `requestAnimationFrame` callback,
- so the next frame's `dtSeconds` is ~280 ms, clamps to 0.25 s again, and
  asks for the maximum again.

Instrumenting `requestAnimationFrame` during a real boot confirmed exactly
that shape -- and confirmed the cost was inside `runCycles`, not in
rendering, audio, or heap growth:

```
frames: 253 over 25s
frame callback duration  p50 5.6   p90 313.8   p99 456.6   max 461.0 ms
total ms:  runCycles 23083   renderFrame 99   canvas 16   audio 3
```

23.1 seconds of a 25-second run inside `runCycles`, in ~300 ms
uninterruptible blocks arriving back to back. The per-frame stall list
showed them every ~300 ms continuously after the 8-second mark, i.e. a 0%
idle duty cycle. A keystroke landing in that window waits for the current
block to finish; one landing while the 8042's single-byte output register
is still full is simply lost (§31 of ibmpc-at's review documents that
register's real behaviour).

Three candidate explanations were measured and **ruled out**:

- **wasm heap growth** (`-sALLOW_MEMORY_GROWTH=1`): the per-frame
  breakdown above accounts for essentially all of the stall time inside
  `runCycles`, leaving nothing for a growth-and-copy pause. Heap growth
  does cause a real stall on this machine, but at page load and power-on
  only -- see §8.5.
- **`ctx.createImageData()` allocating a fresh ~1MB object every frame**:
  real, but 16 ms total across a 25-second run (0.06 ms/frame). It is
  noise, and was deliberately left alone rather than "fixed" on a hunch.
- **a protected-mode fault firing periodically in real mode**: the stalls
  are not periodic at all once measured properly -- they are continuous.
  There was no ~1 Hz signal to explain.

### 8.2 Where the time actually went

Profiled natively (`sample`, `-O2 -g`, same FreeDOS image booted to the
same idle prompt). Top of stack, 5134 samples:

```
cpu80486::Cpu::step_inner()          1070
cpu80486::Cpu::translate()            553
pc486::Pic8259::highest_pending()     435   <-- not CPU work
cpu80486::Cpu::seg_linear()           426
pc486::Pit8253::tick()                273   <-- not CPU work
std::__function::__func<...>::op()    247   <-- the memory bus
pc486::Chipset::tick()                207   <-- not CPU work
pc486::Fdc765::tick()                 206   <-- not CPU work
pc486::Ega::tick()                    148   <-- not CPU work
pc486::Wd1003::tick()                 125   <-- not CPU work
pc486::AtapiCdrom::tick()             101   <-- not CPU work
pc486::PcSpeaker::update()             82   <-- not CPU work
pc486::Chipset::poll_interrupt()       62   <-- not CPU work
```

**About 36% of all execution time was chipset bookkeeping, not 80486
emulation**, plus another ~7% in the `std::function` memory bus. The cause
is structural: `Machine::run_cycles()` calls `chipset.tick()` and
`chipset.poll_interrupt()` after *every single instruction*, and every one
of those device ticks lived in its own `.cpp` file, so each was an
out-of-line cross-translation-unit call whose body is two loads and a
not-taken branch.

### 8.3 The fix: make observing the chipset cheap

Nothing here changes *when* a device is observed or *what* it reports. The
emulated machine's behaviour is identical -- the strongest evidence being
that after all of it, `hdd-boot-check` reaches its idle `C:\>` prompt at
**cycle 1,409,073,889, the same cycle as before**, bit for bit.

- **`Pic8259::highest_pending()`** was an 8-iteration loop looking for the
  lowest set bit of `irr_ & ~imr_`, called (twice, via both PICs) after
  every instruction. This chip is fixed-priority with IR0 highest, so the
  answer is exactly `__builtin_ctz`. Moved inline into the header.
- **`Machine::run_cycles()`** now guards the out-of-line
  `chipset.poll_interrupt()` behind the inline `chipset.has_interrupt()`.
  This is exact, not an approximation: when neither PIC has an unmasked
  pending line, `poll_interrupt()`'s only side effect is `lower(2)` on an
  already-clear master IR2 -- and if IR2 *were* set, the master would
  report a pending interrupt and the guard would let the call through.
- **`Pit8253::tick()`** computed `PIT_HZ / cpu_hz` -- a floating-point
  division -- 66 million times a second, for a `cpu_hz` that never changes.
  Memoized.
- **`Fdc765::tick`, `Wd1003::tick`, `AtapiCdrom::tick`, `Ega::tick`** moved
  from their `.cpp` files into their headers so they inline at the one call
  site that matters. Their bodies are unchanged.
- **`cpu80486::Bus`** held six `std::function`s. Every guest memory byte the
  interpreter touches crosses that boundary, and `std::function` costs a
  second indirect call through its type-erased thunk and blocks inlining at
  every access. Replaced with plain function pointers plus a `ctx` pointer,
  built through the new `Bus::For(host)` helper. Same architecture, same
  call sites (via `Cpu::bus_read`/`bus_write`/...), no type erasure.
- **`web/Makefile`: `-O2` -> `-O3 -flto`.** The hot loop is spread across
  `machine.cpp`, `cpu80486.cpp`, `chipset.cpp` and each device's own
  translation unit; without link-time optimization none of the above can
  actually inline across those boundaries. Worth ~10% on its own.

Measured result, same benchmark as §8.1:

|                        | before        | after          |
| ---------------------- | ------------- | -------------- |
| wasm, idle prompt      | 59.2 M cyc/s  | **76.5 M cyc/s** |
| native, idle prompt    | 84.4 M cyc/s  | **103.7 M cyc/s** |

76.5 M/s against a 66.0 M/s requirement: the machine can now hold real time
with ~16% headroom, where before it was 10% short.

### 8.4 The fix that matters more: never block the main thread

Raising throughput above 66 M/s is necessary but **not sufficient**, and
relying on it alone would have been the wrong call. Any host slower than
this one -- and any workload heavier than an idle shell, which includes
this machine's entire reason for existing -- puts the emulator right back
into the state §8.1 describes. Measured after §8.3's work but before this
change, the boot sequence (a heavier instruction mix than the idle prompt)
still produced 150-350 ms blocks for its first ~15 seconds.

So `app.js`'s main loop is now split in two:

- **`pump()`** advances the machine, in chunks bounded by *real wall-clock
  time* (`kChunkMs = 12`), yielding to the event loop between them.
- **`frame()`** (`requestAnimationFrame`) only draws the screen and LEDs.

The yield is a `MessageChannel` message, deliberately: `queueMicrotask` and
`Promise.resolve()` run the next chunk in the same event-loop turn and
dispatch no input at all, while `setTimeout(0)` is clamped to ~4 ms, which
would itself cap emulated speed.

The chunk budget is a cycle count derived from measured host throughput
(an EWMA seeded at the real 66 MHz rate), so it follows the machine the
page is actually running on. **Cycles over budget are dropped, not
banked**: a host that cannot sustain the requested rate would never work a
backlog off, and each attempt to try is exactly the long, input-starving
call this bound exists to prevent.

That last point is a genuine departure from the real hardware and is
recorded as one: on a host that cannot keep up, emulated time runs slow. It
is the deliberate choice -- a 486 running 5% slow is closer to the real
article than a 486 whose keyboard stops answering. On a host that *can*
keep up, nothing is dropped and the clock is exactly real, which is what
`tests/smoke.spec.ts` continues to verify.

Measured after, over a 40-second run (2424 frames), with
`PerformanceObserver('longtask')` -- which catches main-thread blocking
regardless of which loop caused it:

```
runCycles chunk duration   p50 0.1   p90 0.2   p99 0.7   max 14.0 ms
rAF frame gap              p50 16.6  p99 24.8  max 120.1 ms
long tasks (>50ms)         2 in 40 seconds  (0.5s = 120ms, 20.1s = 65ms)
```

Against the before numbers (p99 456 ms, max 461 ms, a stall every frame):

| frame-time          | before   | after     |
| ------------------- | -------- | --------- |
| p50                 | 5.6 ms   | 16.6 ms (a clean 60 Hz) |
| p99                 | 456.6 ms | **24.8 ms** |
| max                 | 461.0 ms | **120.1 ms** (page load; see §8.5) |
| blocking > 50 ms    | ~every frame | **2 in 40 s** |

Emulated speed, measured at the idle prompt after boot settles:
**66.1 M cycles/sec against a real 66.0** -- real time, exactly. Across a
whole run including boot it averages 63.9 M/s, the shortfall being the boot
sequence, where this host genuinely cannot sustain 66 MHz and the pump
correctly sheds rather than freezing.

`tests/keyboard.spec.ts` went from **0/5 to 5/5**, and the full front-end
suite is **42/42**.

### 8.5 Two real bugs this uncovered, neither of them performance

Both were found because the page now gets far enough to hit them.

**The CD-ROM was never actually in the drive.** `powerOn()` passed
`firmware.cdrom` -- the raw `ArrayBuffer` `fetch()` returns -- straight to
`mountCdrom()`. That goes through embind's `convertJSArrayToNumberVector`,
which reads `.length`; an `ArrayBuffer` only has `byteLength`, so it
converted to an **empty** vector and the drive came up with no disc in it,
silently. The HDD path one line above already wrapped in `new
Uint8Array(...)`. Proven directly in the live page:

```
mountCdrom(new ArrayBuffer(8192)) -> cdromPresent() === false
mountCdrom(new Uint8Array(8192))  -> cdromPresent() === true
```

**Every power cycle leaked ~950 MB of wasm heap.** `powerOff()` set
`machine = null`, but an embind handle owns a C++ object that outlives the
JS reference -- dropping it frees nothing. Each `powerOn()` built another
`Machine` holding another 528 MB C: image and 419 MB CD, and the second
power-on died trying to grow the heap past it, throwing an uncaught wasm
exception that left the page half-powered. `powerOn()` now `delete()`s the
previous cycle's handle before building the next one -- deliberately there
rather than in `powerOff()`, so the handle stays callable while the machine
is off, which is how `tests/boot.spec.ts` observes that power-off really
does stop the cycle counter.

This leak is also the honest answer to "is `-sALLOW_MEMORY_GROWTH=1` a
problem?" -- yes, but at power-on, not per frame. The one remaining >50 ms
long task at page load (120 ms) is wasm instantiation plus mounting 947 MB
of disc images, which is genuinely unavoidable work at that point.

### 8.6 Known remaining issues, not fixed here

- **`persistHddIfDirty()` copies all 528 MB of C: to take a snapshot.**
  This is the 65 ms long task at the 20-second mark above. It fires at most
  once per 5 s and only when C: has actually been written, so it is well
  inside the responsiveness bar, but it is the largest remaining avoidable
  stall and wants incremental (dirty-sector) persistence rather than a
  whole-image copy.
- **`Ega::tick`'s frame period is still `8000000.0 / 60.0`** -- the *AT's*
  8 MHz clock, inherited verbatim from `ibmpc-at`, not this machine's
  66 MHz. The emulated vertical retrace therefore cycles at roughly
  60 x 66/8 = 495 Hz instead of 60 Hz. This is a genuine fidelity bug and is
  flagged, not fixed: correcting it makes every "wait for vertical retrace"
  loop wait ~8x longer in emulated time, which is a real behavioural change
  that deserves its own verification pass rather than riding along with a
  performance fix. The constant is now commented in `ega.h` so it cannot be
  mistaken for intentional.
- **`TEST_CPU_MULTIPLIER = 20` is not achievable and never was.** This host
  tops out near 76 M cycles/sec; 20x of 66 MHz would be 1.3 billion. Under
  the old loop, asking for it produced multi-second synchronous blocks for
  **no speed benefit at all** (the throughput was host-bound either way) --
  which is the direct cause of `keyboard.spec.ts` timing out at the full
  120 s on a single click. With §8.4's chunk budget the excess is simply
  shed, so `fast=1` now means "run as fast as this host can", which is both
  honest and harmless. The suite's wall-clock time is unchanged, because
  the achieved throughput was always the host ceiling.
- One front-end test was corrected rather than the code: `hdd.spec.ts`'s
  "shows the factory-default label ... **on first load**" was waiting for a
  full boot first. FreeDOS genuinely writes to C: while running
  `FDAUTO.BAT`, measured at 12.9 s -- about 2 s of real time *before* the
  `C:\>` prompt appears at 15.0 s -- so after a completed boot the drive has
  correctly been relabelled "saved state (changes from this session)" and
  the assertion was racing the 5 s persist timer. It now asserts on first
  load, as its name says.

### 8.7 What this says about the test strategy

Every native proof this machine has passed, and kept passing, throughout.
The bug was never in the emulated hardware -- it was in the delivery
vehicle, and it made the product unusable.

`tests/smoke.spec.ts` already measured sustained cycles/sec at real speed
against a 33-99 M/sec tolerance band, and it passed the whole time: at
59 M/sec the machine was inside that band while being 10% too slow to keep
up, and the band says nothing at all about *how the work is distributed*.
Real-time emulation has two requirements, and the suite only checked one:

1. enough throughput (`smoke.spec.ts` covers this, loosely), and
2. no single synchronous block long enough to starve input (**nothing
   covered this**).

A regression test for (2) belongs in this suite -- a `longtask`
PerformanceObserver over a real boot, asserting no task exceeds ~100 ms --
and is the obvious next addition.

## 9. Milestone 4: BOOM, and one undefined shift that moved a page table to address zero

Milestones 2 and 3 were each proved against something this repo wrote --
`pm_stub_check`'s hand-assembled DOS4GW-style stub (§6.9) and
`vbe_mode13_check`'s hand-assembled boot sector (§7). This section is the
first time the machine is held to genuine third-party protected-mode
software: **BOOM**, a DOS Doom source port built with DJGPP, loaded by the
GO32-V2 stub through the **CWSDPMI** DPMI host, running **FreeDoom** off
`C:\GAMES\BOOM\DOOM.WAD` (17,659,828 bytes). That exercises paging, a DPMI
host's own #PF handler, the LDT, ring 3, and mode 13h at once, none of it
written to this emulator's expectations.

The reported symptom was CWSDPMI's own page-fault diagnostic screen --
`Page Fault cr2=00400006 ... error=0004` -- after a correctly rendered
FreeDoom title screen. `error=0004` decodes as P=0/W=0/U=1: a ring-3 read of
an absent page, which is ordinary demand-paging territory a DPMI host is
supposed to resolve. The natural reading was a paging or TLB bug in
`cpu80486.cpp`.

**That reading was wrong, and the evidence says so plainly.** Paging was
never broken. In the run that now succeeds, CWSDPMI takes and services
**2992 page faults** through its own IDT vector-14 handler, and the game
renders. The bug was three layers upstream, in real mode, before CWSDPMI had
even entered protected mode -- and it was an *undefined* instruction case.

### 9.1 The fact: a 64KB block of zeros, executed

`disks/boom_run_check.cpp` (`make boom-check`) boots the shipped HDD image,
types `cd \games\boom` and `boom`, and watches. The first native run
reproduced nothing like the browser's report: BOOM printed **no output at
all**, never reached graphics, and sat there for 40 billion cycles (about
ten minutes of emulated time).

A sampled CS:EIP histogram -- §5.9's technique -- showed why:

```
[  3487 Mcyc] mode=0 cs:eip=3725:0000D11E cpl=0 pg=0  hot:
   3725:00005310=10173 3725:00007310=10173 3725:00009310=10173
   3725:0000B310=10173 3725:00001310=10172 3725:00003310=10172
```

A perfectly regular grid of hot addresses 0x2000 apart, each hit the same
number of times, is not a loop -- it is straight-line execution sweeping
through memory and wrapping at the 64KB segment boundary. The instruction
ring buffer confirmed it exactly:

```
3725:000012C2 lin=00038512 cpl=0 pg=0 ... | 00 00 00 00 00 00 00 00 00 00
3725:000012C4 lin=00038514 cpl=0 pg=0 ... | 00 00 00 00 00 00 00 00 00 00
```

`00 00` is `ADD [BX+SI],AL`. The CPU was grinding through zeroed memory two
bytes at a time with its registers frozen. Control flow had gone somewhere
that does not exist -- the same signature §5.9 recorded for JemmEx, reached
by a completely different route.

### 9.2 Walking it back: the IVT had become a page table

Catching the *first* few zero-instructions (rather than noticing the spin
billions of cycles later) keeps the preceding real instructions in the ring.
The last real instruction before the runaway was:

```
1632:00000B03 ... eax=00006240 ... | CD 21 ...      ; INT 21h, AH=62h
```

-- an ordinary DOS call, vectoring straight into zeroed memory at
`0002:1007` (linear 0x1027). So either the vector was corrupt or the handler
had been wiped. Comparing the live IVT against a snapshot taken when BOOM
was launched settled it in one line each:

```
INT 00: 00D9:12BA -> 0000:0207
INT 01: 00D9:1285 -> 0000:1007
INT 02: 0070:0008 -> 0000:2007
INT 10: C000:7D70 -> 0001:0007
INT 13: F000:E3FE -> 0001:3007
```

Every vector, replaced by a regular pattern. Decoded as a dword, vector `V`
now holds `V * 0x1000 + 7` -- which is not an interrupt vector at all. It is
a **page-table entry**: physical frame `V`, flags `P|W|U`. (Vector 0 reads
`0x00000207` rather than `0x00000007` because bit 9 of a PTE is one of the
three bits Intel leaves available to software, and CWSDPMI had already
marked that page; every other entry is the bare pattern.) CWSDPMI had built
its identity-mapping page table at **physical address 0**, on top of the
interrupt vector table and everything DOS keeps under it.

### 9.3 The NULL far pointer, and the instruction that produced it

A watchpoint on linear 0x84 (the INT 21h vector) named the writing loop:

```
1632:2F75  MOV  AX, DI
1632:2F77  SHL  AX, 2
1632:2F7A  LES  BX, [0x0376]      ; far pointer to the page table
1632:2F7E  ADD  BX, AX
1632:2F80  MOV  word ES:[BX+2], 0
1632:2F86  MOV  word ES:[BX], 0
1632:2F90  INC  DI
1632:2F91  CMP  DI, 0x400         ; 1024 entries
```

`ES` was `0000` with an ordinary real-mode limit of `FFFF`, and `CR0` was
`0x00000010` -- no unreal mode, no protected mode. The far pointer at
`[0x376]` was simply `0000:0000`, so a routine zeroing 4KB of page table
zeroed the bottom of memory instead.

The store that produced it is six instructions long, and every one matters:

```
1632:2F49  CALL ...        -> AX = 0x2B        ; a physical page number
1632:2F4C  XOR  DX, DX
1632:2F4E  MOV  CL, 0x18                       ; 24
1632:2F50  CALL 1632:0184                      ; DX:AX <<= CL
1632:2F53  MOV  [0x0378], DX                   ; far pointer segment
1632:2F57  MOV  [0x0376], AX                   ; far pointer offset
```

and the helper it calls is Borland's 16-bit runtime shift:

```
1632:0184  SHLD DX, AX, CL      ; 0F A5 C2
1632:0187  XOR  BX, BX
1632:0189  SHLD AX, BX, CL      ; 0F A5 D8
1632:018C  RET
```

`0x0000002B << 24` is `0x2B000000`, so `DX:AX` should come back as
`DX=0x2B00, AX=0x0000` -- far pointer `2B00:0000`, linear 0x2B000, page
0x2B. This core returned **zero**. (The allocation itself was fine and the
trace proves it: `AH=48h BX=0C40h` returned segment `0x2026` with CF clear,
and the resize to `0x0BDA` paragraphs succeeded, so the block runs from
0x20260 to 0x2C000 -- page 0x2B sits inside CWSDPMI's own memory, exactly
where it belongs.)

### 9.4 The bug: a negative shift, in the one case Intel calls undefined

`Cpu::shld()`'s 16-bit path computed the SHLD result as

```cpp
res = uint16_t(pair >> (16 - count));     // pair = (dst << 16) | src
```

For a count of 1-16 that is right, and it is just the standard identity:
shift the 32-bit `dest:src` concatenation left by `count` and keep the high
half. For a count above 16 -- 24, here -- `16 - count` is **negative**, which
is undefined behavior in C++; in practice the shift amount is masked to five
bits, so `pair >> 24` ran instead and produced 0.

Intel's text is what makes this easy to get wrong. The 80486 PRM's SHLD/SHRD
entry says the count is masked to 5 bits and that **a count greater than the
operand size leaves the result undefined**, so a 16-bit double shift by 24 is
formally undefined and a core is entitled to do anything.

But "undefined in the manual" is not "arbitrary in the silicon", and this
repo's rule (CLAUDE.md: a real chip quirk is worth preserving even when it
looks like a bug, because software depends on it) applies exactly here. The
486 has one 32-bit shifter. It masks the count to 5 bits, shifts the 32-bit
`dest:src` concatenation, and keeps the half the instruction names -- so
above a count of 15, bits of the *source register* land in the destination.
The fix implements precisely that, computing in 64 bits (a 32-bit
`pair << count` would overflow) and narrowing to 32, the real shifter's
width, so bits shifted off the top are lost the way hardware loses them:

```cpp
cf  = ((pair >> (32 - count)) & 1) != 0;
res = uint16_t(uint32_t(pair << count) >> 16);
```

**The strongest evidence that this is the hardware's actual behavior is the
guest code itself.** Borland's helper is `SHLD dx,ax,cl / XOR bx,bx /
SHLD ax,bx,cl`, with a SHRD mirror for right shifts, and it is compiled in as
*the* implementation of `unsigned long << n` for a variable `n`. It is
correct across the whole 0-31 range only under this behavior, and CWSDPMI
calls it with `cl=24`. A helper written to be correct for counts 16-31 pins
the semantics as surely as a manual would; period compilers shipped it
because real 486s did this.

Worth recording which half was already right: the **SHRD** path computed
`pair >> count` with `pair = (src << 16) | dst`, which is well defined for
every count up to 31 and already matched hardware. Only SHLD's `16 - count`
went negative. That asymmetry is why the failure was so selective -- and why
`hdd-boot-check` had been passing throughout.

### 9.5 Why the two symptoms differed, and what was never wrong

The browser showed a rendered title screen and then a CWSDPMI page fault at
`cr2=00400006`; natively the same build produced a runaway into zeros with no
output at all. Both are the same root cause reached through undefined
behavior, which is free to differ between builds: the native build is `-O2`,
the wasm build `-O3 -flto` (§8.3), and a negative shift count is exactly the
kind of construct two optimizers can fold differently. `cr2=0x400006` is
itself consistent with the wreckage rather than with a paging bug: DJGPP
links its image from 0x1000 up and deliberately leaves the first page
unmapped so a NULL dereference traps, so a fault six bytes into that guard
page is a near-NULL pointer in the client -- the downstream effect of a page
table built in the wrong place, not a translation failure.

Three things the evidence clears outright, all of which were prime suspects:

- **Page-fault delivery is correct.** The successful run takes 2992 #PFs and
  CWSDPMI services every one through its own vector-14 handler.
- **The TLB and INVLPG are correct.** Those 2992 repairs only work if a
  handler's page-table update is visible afterwards.
- **Safe Mode (§5.9, HIMEMX-only, no JEMM386) is not implicated.** CWSDPMI
  asks DOS for memory, not the memory manager: `AH=48h` returned a real
  segment with CF clear, and the trace shows the whole `5800/5802/5801/5803`
  allocation-strategy dance completing normally. It needed no service Safe
  Mode withholds.

### 9.6 The proof

`make boom-check` boots the shipped image, runs BOOM, and requires the game
to reach its renderer. The bar is deliberately not "a picture appeared" --
the title screen is a single static bitmap that a working WAD loader alone
would produce -- but **24 consecutive differing 320x200x256 frames**, which
only a running 3D renderer produces:

```
[  1601 Mcyc] > IWAD found: ./doom.wad
[  1627 Mcyc] > R_Init: Init DOOM refresh daemon -
[  2593 Mcyc] > P_Init: Init Playloop state.
[  2595 Mcyc] > I_InitSound:
[  2823 Mcyc] > HU_Init: Setting up heads up display.
[  2825 Mcyc] > ST_Init: Init status bar.

OK: BOOM is rendering -- 24 consecutive differing 320x200x256 frames,
25 distinct frames total, at cycle 3241155494
faults during the run: v7=3 v14=2992
```

The captured frames are the rest of the proof: the title screen mid-**melt**
(Doom's column-by-column wipe, jagged by design, with FreeDoom's art and URL
legible through it), and gameplay -- a 3D-rendered room, blue nukage floor,
three enemies, status bar. `v7=3` is #NM from CR0.TS across task switches,
which is the FPU behaving as §6.6 describes.

Regression coverage, in `tests/cpu80486_test.cpp`:

- `SixteenBitShldAboveFifteenShiftsTheThirtyTwoBitConcatenation` -- the exact
  case CWSDPMI executes (page 0x2B, count 24), plus the whole Borland helper
  run as a sequence proving `DX:AX <<= 24`.
- `SixteenBitShrdAboveFifteenShiftsTheThirtyTwoBitConcatenation` -- the SHRD
  mirror, `DX:AX >>= 24`, which was already correct and now cannot regress.
- `SixteenBitShldAndShrdWithinTheDocumentedRange` and
  `SixteenBitDoubleShiftCarryIsTheLastBitShiftedOut` -- the specified range
  and the carry rule, pinning down that widening the pair to 64 bits changed
  no documented case.

Suite: **415 GoogleTest cases pass** (411 before, plus these four), with
`pm-check` 24/24, `vbe-check` all-pass, and `hdd-boot-check` reaching its
idle `C:\>` at **cycle 1,409,073,889** -- the identical figure §8.3 recorded,
so real mode is still cycle-for-cycle unchanged.

### 9.7 The tooling this needed, and what it costs

None of §9.1-9.3 was reachable with the diagnostics that existed. `on_fault`
and `on_unimplemented` (§4.7, §6.4) report a fault or a bad opcode, and this
bug produced neither -- the guest was executing perfectly valid `ADD
[BX+SI],AL` instructions over zeroed memory. What it needed was an
instruction ring buffer, which meant a per-instruction hook in
`Machine::run_cycles()` -- §8's hot loop, where a `std::function` would not
be affordable. It is a plain function pointer plus a ctx pointer, the same
shape §8.3 gave `cpu80486::Bus`, and null by default.

Measured rather than asserted, since §8 is the reason to care: a full
`hdd_boot_check` boot runs in **15.07 s with the hook present and null
against 14.92 s with the call removed entirely** (native, `-O2`, two passes
each, idle machine) -- about 1%. An early figure of 21.5 s was contention
from a Playwright suite running in parallel, which is worth recording as its
own lesson: the first A/B measurement was wrong by 44% because the machine
was busy, and re-running it on an idle host was the difference between
"unacceptable regression" and "noise".

Three diagnostics built on that hook did the actual work, and all three are
in `boom_run_check.cpp` behind flags:

- `--catch-runaway` dumps the ring the moment the guest starts executing
  zeroed memory. Timing is the whole point: noticing the spin billions of
  cycles later finds a ring full of zeros, while catching the eighth such
  instruction still holds ~24000 real ones.
- `--watch LINEAR` dumps the ring when a linear dword changes, so the
  instruction that wrote it is the newest entry. This is what named the
  zeroing loop from the corrupted INT 21h vector.
- An IVT snapshot taken at launch, diffed at the failure, which is what
  turned "the vectors look wrong" into "these are page-table entries".

### 9.8 Two smaller findings, recorded rather than fixed

- **The reported text-mode corruption does not reproduce.** The garbled
  colored blocks in a directory listing and in the `C:\GAMES\BOOM>` prompt
  were checked directly on the fixed core: rendering a real `DIR` of
  `C:\GAMES\BOOM` through `ega_render.cpp` gives a clean screen -- every
  digit of the file-size column, every glyph of the prompt, correct
  attributes. The most plausible account is that those screenshots came from
  a session in which BOOM had already run and this bug had already page-
  tabled over the IVT and DOS's own data at physical 0, leaving DOS drawing
  from corrupted state; that is an explanation consistent with the evidence,
  not a demonstrated one, and it is worth re-checking in the browser before
  treating it as closed.
- **`FF /7` is silently ignored where a real 486 raises #UD.** `grp5()`'s
  `default` arm reports the opcode through `on_unimplemented` and returns,
  rather than faulting; Intel documents `FF /7` as an invalid encoding. Every
  occurrence observed here was an artifact of the runaway executing garbage
  (all at `0000:xxxx` and `0002:xxxx`, inside the wreckage), and all of them
  disappear with the fix, so nothing real depends on it -- but a core that
  silently continues past an invalid opcode lets a runaway guest grind on
  where hardware would have stopped it, which is the opposite of helpful.
  Fixing it wants its own verification pass against the boot path.

### 9.9 What this says about the test strategy

The suite was 411 cases green, `pm-check` 24/24, `vbe-check` all-pass and
`hdd-boot-check` cycle-exact, throughout -- while a core instruction returned
zero for an input real software actually uses. Every one of those proofs was
written against code this repo controls, and none of it happens to do a
16-bit double shift by more than 15.

That is the general shape: the cases a hand-written test suite omits are
precisely the ones nobody thought of, and undefined-in-the-manual corners are
where period software quietly depends on real silicon. Running genuine
third-party software is not a nice-to-have on top of the unit suite -- it is
the only thing that samples the instruction set the way 1990s compilers
actually emitted it. `make boom-check` is now a standing member of that
family alongside `hdd-boot-check`, for the same reason.

### 9.10 Sources

- Intel 80486 Programmer's Reference Manual (1990/1992) -- SHLD/SHRD: the
  5-bit count mask, the fill-from-source definition, the carry rule, and the
  statement that a count greater than the operand size leaves the result
  undefined. Also `FF /7` as an invalid encoding (§9.7).
- **CWSDPMI r5 and its Borland-compiled runtime helpers, as shipped in
  `C:\GAMES\BOOM\CWSDPMI.EXE`** -- cited as the primary evidence for the
  above-15 behavior: a 32-bit shift helper correct across 0-31 only under
  the 32-bit-concatenation semantics, called with `cl=24`. The disassembly
  in §9.3 is read straight out of this machine's own instruction trace.
- DJGPP's image layout -- text linked from 0x1000 with the first page left
  unmapped as a NULL-pointer guard, which is what makes `cr2=0x400006`
  readable as a near-NULL client dereference (§9.5).

## 10. Milestone 4: a PS/2 mouse on the 8042's second port

Every machine before this one had exactly one input device. This milestone
adds the other one a 1993-94 "maxed out" 486 shipped with -- a PS/2 mouse
hanging off the keyboard controller's auxiliary port. It is a device-level
change, contained to `i8042.h`/`i8042.cpp` and its own suite: the AUX side
of the controller, IRQ12, and a complete standard PS/2 mouse behind it.

### 10.1 The second port is a PS/2 feature, not an AT one

`i8042.cpp` models a *PC/AT* keyboard controller, and the original AT's own
8042 firmware has no mouse in it at all. OS/2 Museum's disassembly of the
5170 KBC ROM lists commands 80h-A8h and D2h-DEh as **ignored** outright --
which covers 0xA7/0xA8 (disable/enable AUX), 0xD2/0xD3 (write to either
output buffer) and 0xD4 (write to the mouse) -- and status-register bit 5 on
that part means "transmit timeout", not "this byte came from the mouse".

The port arrived with the PS/2 line in 1987 and was universal on 486 boards
by the time this machine is built: the round mouse DIN next to the keyboard
DIN is the same 8042 answering a superset of the same command set. So the
class keeps its name and its AT wiring, and gains the PS/2 superset on top
-- the same "this is the later part that is backward-compatible with the
documented one" position `wd1003` already occupies for IDE.

### 10.2 Status bit 5 is the whole mechanism, and it is not optional

One output buffer, one data port, two devices behind it. Everything above
the controller separates the two streams by **AUXB**, status bit 5: set
means the byte in the buffer came from the mouse.

This is not a detail that degrades gracefully if you get it wrong. The
BIOS this machine ships reads mouse bytes through exactly one helper:

```c
get_mouse_data(data)
{
  while ((inb(PORT_PS2_STATUS) & 0x21) != 0x21) { }
  response = inb(PORT_PS2_DATA);
  ...
}
```

-- `rombios.c`. Its INT 74h handler opens with the same test and returns
immediately if it fails. A mouse byte delivered with OBF but without AUXB
is a byte this firmware spins on forever, so bit 5 is load-bearing for
every single transfer, command ACKs included.

### 10.3 What the firmware's own sequence demanded

Three requirements fell straight out of reading `rombios.c`'s INT 15h
AH=C2h implementation rather than the protocol documents, and each is now a
test in `i8042_test.cpp`:

- **Commands must work while the AUX clock is "disabled".** Every C2h
  subfunction begins with `inhibit_mouse_int_and_events()`, which clears
  command-byte bit 1 (IRQ12) and *sets* bit 5 (drive the mouse clock line
  low) -- and then sends mouse commands through 0xD4 and waits for their
  ACKs. Bit 5 stops the device *reporting*; it does not stop the controller
  raising the line to carry a host command. Only unsolicited movement
  packets are gated on it.
- **A write to port 0x64 cancels a pending 0xD4.** `set_kbd_command_byte()`
  writes 0xD4 to 0x64 and then immediately 0x60 to 0x64, abandoning that
  "write to mouse" without ever supplying its data byte. The 8042
  distinguishes command from data by the A2 address line, so the second
  write is simply a new command. Had the stale 0xD4 survived, the command
  byte would have been delivered to the *mouse* -- and the machine would
  have lost its keyboard and mouse interrupts at the exact moment a driver
  turned them on.
- **Every mouse byte raises IRQ12, ACKs included.** That is precisely why
  the BIOS disables IRQ12 around its own command traffic and restores it
  afterwards.

POST itself writes command byte 0x61: translation on, IRQ1 on, **AUX clock
disabled**. So a mouse is inert until a driver (INT 15h C2h, or the command
byte directly) enables it -- which is the correct power-on posture, and
means this device changes nothing about how the machine boots today.

### 10.4 A one-byte buffer needed a real queue behind it

Until now the output buffer was a single byte and a new byte simply
overwrote whatever was unread. That survives a keyboard-only machine
because BIOS drains before it asks anything. It does not survive a mouse:
the smallest unit this device sends is a **3-byte packet**, a reset answers
**FA AA 00**, and a player moving the mouse while typing produces both
streams at once. Overwriting would have silently eaten one byte of every
packet that collided with a keystroke.

Real hardware queues this in the *devices*, not the controller: the 8042
holds a device's clock line low for as long as OBF is set, and the device
keeps its bytes until the line is released. The FIFO added here stands in
for both devices' holding buffers, each byte tagged with its source (which
decides bit 5) and which interrupt line it drives. Answers to *controller*
commands deliberately do not go through it -- the 8042 has no queue of its
own and writes its answer straight into the buffer over whatever was there,
and every caller of those commands polls for the answer.

Two consequences, both of them the real behavior: a scan code can no longer
destroy an unread byte, and a scan code injected while the buffer is full
waits instead of arriving early. Two existing tests were asserting the old
overwrite (one here, one in `chipset_test.cpp`); both now drain the
keyboard's power-on BAT byte first, which is exactly what BIOS POST's own
flush loop does before it tests anything.

### 10.5 The mouse: a period 3-button PS/2 mouse, and no wheel

The device implements the full standard command set -- FF, FE, F6, F5, F4,
F3, F2, F0, EE, EC, EB, EA, E9, E8, E7, E6 -- with its real defaults (100
samples/sec, 4 counts/mm, 1:1 scaling, reporting disabled, stream mode),
all four modes (reset, stream, remote, wrap), and the 3-byte movement
packet. Details worth naming, because each is easy to get plausibly wrong:

- **+Y is away from the user**, not down the screen. The reference's own
  worked table is the test: move up one is `08 00 01`, move down one is
  `28 00 FF`. A front end handing this device browser `movementY` must
  negate it.
- **Bit 3 of byte 1 is always set.** Some drivers ignore it; others treat a
  packet without it as an error and reinitialize the mouse.
- **The counters are 9-bit and saturate.** Range -255..+255; beyond that
  the overflow bit is set and the excess is genuinely lost, not carried.
- **2:1 scaling applies to stream reports only** -- not to the packet
  returned by Read Data (0xEB). The table (0, 1, 1, 3, 6, 9, 2N) is the
  mouse's own, applied to the counters before they are reported.
- **The status packet's buttons are in the reverse order** of the movement
  packet's: left/middle/right in bits 2/1/0, against left/right/middle in
  bits 0/1/2.

Deliberately absent: the Microsoft IntelliMouse's 4-byte wheel packet. That
part shipped in 1996, two years after this machine's build date, and no DOS
software of the era asks for it. Its "knock" (set sample rate 200, 100, 80,
then read device ID) is answered the way a *standard* mouse answers it --
every rate accepted, device ID still 0x00 -- which is exactly how a driver
probing for a wheel learns there isn't one. Adding it later is a
self-contained change if a reason ever appears.

### 10.6 Tests

`i8042_test.cpp` goes from 10 cases to 37. The new ones cover the
controller side (AUXB tagging, 0xA7/0xA8/0xA9, 0xD2/0xD3/0xD4, the
cancelled-0xD4 case, IRQ12 gating and its re-assertion per packet byte),
the device side (reset's three-byte answer, reporting gate, the four
reference movement packets and all three buttons byte-for-byte, counter
saturation and overflow, remote mode, scaling in stream but not in Read
Data, status request at defaults and after programming, set-defaults, wrap
mode's two exceptions, resend, unknown-command 0xFE, counters cleared by
commands), and two integration-shaped cases: keyboard and mouse bytes
interleaving without loss, and the BIOS's own INT 15h AH=C2h init sequence
run end to end.

The bar this milestone is *not* yet at is the same one every milestone here
is judged by: real third-party software. A DOS game reaches the mouse
through INT 33h, which is a driver (FreeDOS ships CTMOUSE), not firmware --
so "BOOM sees the mouse at its setup screen" needs the chipset's IRQ12
line, a host-side event path, and that driver on the shipped image before
it can be claimed.

### 10.7 Sources

- **Adam Chapweske, "The PS/2 Mouse Interface" (2001)** -- the command set
  and every response byte, the reset defaults and BAT/device-ID sequence,
  the four modes, the 3-byte packet layout, the 9-bit counters and their
  overflow rule, the 2:1 scaling table and its stream-only footnote, the
  status-request format, and the worked byte sequences for unit moves and
  button presses that §10.5's tests assert verbatim.
- **Bochs BIOS `rombios.c`** (the ROM this machine actually boots) --
  `get_mouse_data`, `send_to_mouse_ctrl`, `set_kbd_command_byte`,
  `inhibit_mouse_int_and_events` / `enable_mouse_int_and_events`,
  `int15_function_mouse` (AH=C2h) and the INT 74h handler. Cited for what
  the firmware *requires* of the hardware, per §7.11's rule, not as a
  behavioral model.
- **Andries Brouwer, "Keyboard scancodes", ch. 11 (the AT keyboard
  controller)** -- the status register and controller command byte bit by
  bit, and which commands are AT versus PS/2.
- **OS/2 Museum, "IBM PC/AT 8042 Keyboard Controller Commands"** -- the
  5170 KBC ROM disassembly behind §10.1's claim that the original AT part
  ignores the whole AUX command range.
- Bochs `iodev/keyboard.cc` -- cross-check only, on the points where a
  document leaves room (the AUXB flag driving IRQ12, 0xA9's answer going to
  the controller side rather than the AUX side).

## 11. Milestone 4: a Sound Blaster 16 at 0x220, IRQ5, DMA 1 and 5

(§10 is the PS/2 mouse half of this milestone, written separately.)

`soundblaster.h`/`soundblaster.cpp` add the card a 1993-94 "gamer's dream"
486 actually shipped with: a Creative Sound Blaster 16 -- CT1745 mixer, DSP
version 4.05 -- at base I/O 0x220, on IRQ5, with 8-bit DMA channel 1 for
legacy digitized playback and 16-bit DMA channel 5 for its own SB16-mode
stereo. That is exactly the card a period driver describes to itself as
`SET BLASTER=A220 I5 D1 H5 T6`, which is the environment this machine's DOS
software reads.

Everything about the register semantics comes from Creative's own *Sound
Blaster Series Hardware Programming Guide* rather than from another
emulator's source -- see §11.6. Where the guide is silent (a handful of
diagnostic commands Creative never documented but drivers use anyway), the
sources list says so explicitly.

### 11.1 The whole 16-port block, not just the DSP's four

A real card decodes base+0h through base+Fh, so `owns()` claims
0x220-0x22F: FM at base+0..3 and base+8..9, the CT1745 mixer's
address/data pair at base+4/5, DSP reset at base+6, DSP read data at
base+Ah, write command/data (and, on read, write-buffer status) at base+Ch,
read-buffer status at base+Eh, and the SB16's own 16-bit interrupt
acknowledge at base+Fh.

Two decode decisions are deliberate rather than incidental:

- **The 0x388/0x389 alternate FM pair is not claimed at all**, and the FM
  status registers inside the block read back 00h. No FM synthesizer is
  implemented this milestone (it was the plan's explicit stretch goal), and
  the standard AdLib probe -- reset both timers, read status, start timer 1,
  read status again and expect C0h -- therefore fails. That is the point. A
  fake OPL3 that passed detection and then produced silence would be *worse*
  than absent hardware: software would believe its music was playing and
  report success. Absent hardware is a condition period software already
  knows how to handle; a lying status register is not.
- **base+Eh acknowledges the 8-bit interrupt on every read.** Creative is
  explicit that this is how it works ("to remain backward compatible, the
  interrupt acknowledgment of 8-bit DMA mode digitized sound I/O and SB-MIDI
  is done via the Read-Buffer Status port"), which means the same port a
  driver polls during the reset handshake is also the acknowledge. It is not
  a separate register that happens to share an address.

### 11.2 Why DSP 4.05 specifically, and why the version number is load-bearing

The DOS driver BOOM links -- Allegro 3.x's `sb.c` -- reads DSP command E1h,
assembles `(major << 8) | minor`, and branches on `>= 0x400` to choose its
SB16 path: 41h to set a true sampling rate in Hz, then B6h with mode byte
30h (16-bit signed stereo, auto-init, FIFO on) and a sample count, running
on the 16-bit DMA channel. Report 3.02 and the *same driver on the same
card* silently drops to the SB-Pro path instead, and 16-bit stereo output
never gets exercised at all. So the version byte pair is not cosmetic
identification -- it selects which half of this device's command set real
software will ever reach. 4.05 is a genuine shipped SB16 revision.

### 11.3 Command coverage, and the three things deliberately left ignored

Implemented: direct-mode 10h; the legacy 8-bit single-cycle 14h/24h and
auto-init 1Ch/2Ch paced by the 40h time constant with 48h block size; the
DSP 4.xx programmed Cxh (8-bit) and Bxh (16-bit) transfers with their
A/D-vs-D/A, auto-init and FIFO command bits and their stereo/signed mode
byte, rates set by 41h/42h; pause/continue/exit D0h/D4h/D5h/D6h/D9h/DAh;
speaker D1h/D3h/D8h; the 80h silence period; and the identification and
diagnostic commands E0h, E1h, E3h, E4h, E8h, F2h and F3h.

Three groups are accepted and ignored, each for a documented reason:

- **High-speed mode (90h/91h/98h/99h) does not exist on this card.**
  Creative's own availability matrix lists these for DSP 2.01+ and 3.xx
  only, not 4.xx -- DSP 4.xx reaches full rate in normal mode and dropped
  them. Ignoring them is the real SB16's behavior, not a gap in this
  device, and Allegro only issues them after detecting a version below
  4.00 anyway. This is the one place where doing *less* is the accurate
  choice, which is why it has a test of its own.
- **ADPCM (16h/17h/74h-77h, 1Fh/7Dh/7Fh) is decoded far enough to swallow
  its parameter bytes** and no further. Swallowing matters: a driver
  probing 75h writes a command and two length bytes, and a device that
  consumed only the command byte would then read the two length bytes *as
  commands*, desynchronizing the stream and leaving the card in a state no
  real card can be in. A test pins this down.
- **Recording delivers digital silence, paced correctly, with its
  interrupts firing on schedule.** Nothing is plugged into the line or mic
  inputs, so silence is what a real card digitizes -- and note silence is
  not the same byte in both formats (80h unsigned 8-bit, 0000h signed
  16-bit), per Creative's own note that the two formats differ only in
  where minimum amplitude sits. Firing the interrupts is what keeps a
  recording program from waiting forever on a card that is working
  correctly.

### 11.4 Pacing: the same credit scheme as the floppy, for the same reason

Playback is paced by accumulated CPU-cycle credit against the programmed
sample rate, exactly like `fdc765.h`'s data-transfer pacing. Real elapsed
wall-clock time per sample is exact -- CLAUDE.md forbids speeding this up,
and an audio device is the one place where getting it wrong is *inaudible
as a bug and obvious as a symptom*: everything simply plays at the wrong
pitch. The trade-off is the same one the floppy makes: the bytes for a
batch of due samples move in one burst rather than one DMA cycle per
sample, so the DMA address/count registers step in bursts. Software that
waits for the block interrupt (universal practice) cannot tell; software
polling the DMA count to find the play position mid-block would see it
move in steps rather than smoothly. `PlaybackIsPacedAtTheRealSampleRateAndNeverFaster`
is the standing guard on the timing itself.

The audio handed to the front end follows `pcspeaker.h`'s philosophy one
level up. This device does not synthesize anything: it records what the DAC
actually latched and the CPU cycle it latched at, normalized to signed
16-bit stereo (the card's own widest native format). That keeps the core
from committing to an output sample rate, and it makes a program that
changes rate mid-stream, or plays 8-bit mono right after 16-bit stereo,
need no special case at all -- the timestamps already say what happened
when. The CT1745's Voice and Master attenuators are exposed as linear gains
*beside* the sample stream rather than folded into it, so `drain_samples()`
returns exactly the values the program wrote.

### 11.5 Two wiring details the chipset has to get right

Both are called out in `soundblaster.h`'s header because getting either
wrong fails quietly:

- **16-bit transfers run on DMA2's channel 5, whose registers count words,
  not bytes.** The 8237's address outputs drive A1-A16 on the 16-bit
  channels and the page register supplies A17-A23, so the physical address
  is `(page << 16) | (address << 1)`, the available byte count is
  `(count + 1) * 2`, and one `advance()` covers a byte *pair*. Page-register
  bit 0 is not connected on those channels. Treating channel 5 like the
  floppy's channel 2 would put every 16-bit buffer at half its real address.
- **Auto-init playback needs the DMA controller to reload address and count
  from its base registers at terminal count.** `Dma8237` now models this
  directly (§12.2) -- a real 8237A-5 channel latches whatever address/count
  it's programmed with into a second, hidden pair of registers, and on
  autoinit reloads the *working* registers from that hidden pair at
  terminal count instead of continuing to increment/wrap (Intel 8237A-5
  data sheet, "Autoinitialize"). The floppy, the only previous DMA client,
  never set the mode byte's autoinit bit, so this was previously unexercised
  rather than unneeded. The device keeps requesting bytes until the driver
  sends DAh (8-bit) or D9h (16-bit), which is the entire point of auto-init
  mode and is how a period driver plays continuous double-buffered sound.

### 11.6 Sources

- **Creative Technology Ltd, *Sound Blaster Series Hardware Programming
  Guide*** -- the primary source for essentially everything here, and cited
  by chapter/page in both the device header and the test file: Table 2-1's
  DSP I/O ports; chapter 2's reset handshake (write 1, wait 3us, write 0,
  poll read-buffer status bit 7, read 0AAh), interrupt acknowledgment via
  base+Eh/base+Fh, and the mixer 80h/81h/82h interrupt-setup, DMA-setup and
  interrupt-status bit layouts; chapter 3's time-constant formula
  `65536 - 256000000/(channels * rate)` with only the high byte programmed,
  and the single-cycle vs auto-initialize transfer procedures for both the
  legacy 8-bit commands and the DSP 4.xx ones; chapter 4's CT1745 register
  map, per-register power-on defaults, and the statement that the
  CT1345-compatibility volume registers "are actually mapped to the new
  volume control registers"; chapter 6's command reference, including the
  Bxh/Cxh command-byte bit fields (A/D vs D/A, auto-init vs single-cycle,
  FIFO) and mode-byte bit fields (stereo, signed), the availability matrix
  that rules high-speed mode out on 4.xx, and the note that on DSP 4.xx
  D1h/D3h move only the flag D8h reports; and Appendix A's confirmation
  that 220h is the factory default base address for every Sound Blaster.
- **Allegro 3.x `sb.c`** (the DOS Sound Blaster driver BOOM links through
  DJGPP) -- cited as evidence of what a period game's driver *actually
  issues*, rather than what the guide permits: the `(major << 8) | minor`
  version test against 0x400, `256 - 1000000/rate` as the time-constant
  byte, `0xB6` with mode `0x30` for the SB16 path, `0x14` for single-shot
  on pre-2.0 cards, `0x48` + `0x90` on 2.01-3.xx, D1h/D3h for the speaker,
  and acknowledging via base+Eh for 8-bit and base+Fh for 16-bit.
- **`SET BLASTER=A220 I5 D1 H5 T6`** -- the period-standard environment
  string, corroborating 0x220/IRQ5/DMA1/DMA5 as the defaults this device
  powers up with (mixer 80h = 02h, mixer 81h = 22h).
- **DOSBox / DOSBox-X `sblaster.cpp`** -- cited *only* for the exact text
  of the E3h copyright string real cards return
  (`COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.`) and, together with
  contemporary community DSP command tables, for the behavior of the
  commands Creative never documented: E0h returning the bitwise complement
  of its parameter, E4h/E8h as a diagnostic register that survives a DSP
  reset, and F2h/F3h as the 8-bit/16-bit interrupt triggers a driver's IRQ
  auto-detection uses. These are flagged as second-hand rather than
  folded in with the guide's own material, per this repo's rule that
  another emulator's behavior is not itself period-accurate evidence.
- **DMX (Paul Radek), as used by DOOM** -- background on why ~11 kHz 8-bit
  unsigned mono is the format the digitized-output path most needs to get
  right for this era, and why auto-init DMA (rather than a chain of
  single-cycle blocks) is the mode that matters.

## 12. Milestone 4: wiring the mouse and Sound Blaster into the chipset

§10 and §11 build the two devices in isolation, each against its own
independent test suite and neither touching the other's files. This section
is the integration step that follows: getting both talking to `Chipset`,
`Machine`, and the browser front end without the two independently-built
pieces colliding, plus one hardware-accuracy gap the Sound Blaster work
surfaced in a device that predates this milestone.

### 12.1 Port decode, reset, and the tick order

`chipset.h` gains a `SoundBlaster sb` member alongside the existing
`PcSpeaker speaker`, at its real factory-default address (0x220, IRQ5, DMA1/
DMA5 -- §11's `SET BLASTER=A220 I5 D1 H5 T6`). `io_in`/`io_out` add an
`sb.owns(port)` check beside every other device's, and `reset()` resets it
alongside the rest of the chipset on both power-on and the front-panel
Reset button.

The mouse needs no new member: `I8042` already owns the AUX port (§10), so
the only new chipset-level surface is a one-line forwarding method,
`Chipset::inject_mouse_event(dx, dy, buttons)`, that the host calls into
`kbc.inject_mouse_event(...)` -- the same shape as the existing
`injectScancode` path for the keyboard side of the same controller.

`tick()` services both devices every CPU instruction, same as every other
chipset device, but the two need opposite edge-detection policies, and
getting that backwards for either one silently breaks it:

- **IRQ12 (mouse) is level-checked, not edge-detected.** `I8042::
  irq12_pending()` stays asserted across every tick of a multi-byte AUX
  packet until the guest's ISR reads the byte that clears it (§10.4); an
  edge-detect here would raise the interrupt once and then silently drop
  bytes 2 and 3 of every packet, since nothing would re-notice the line is
  still high.
- **IRQ5 (Sound Blaster) is edge-detected**, `sb_irq_prev_` tracking the
  0->1 transition exactly like the existing `fdc_irq_prev_`/`kbc_irq_prev_`/
  `hdd_irq_prev_`/`cdrom_irq_prev_` pattern (see `chipset.h`'s own comment
  on why re-raising a level-triggered ISA line on every tick, rather than
  only its rising edge, causes a real interrupt storm on the PIC). Unlike
  the mouse's AUX port, nothing about the DSP's interrupt-status protocol
  needs the line to stay asserted between ticks -- it's cleared once, by
  the base+Eh/base+Fh read §11.1 describes, not re-derived from live state.

### 12.2 Making DMA autoinit real, in `Dma8237` rather than around it

§11.5 flagged that `soundblaster.h` needs auto-init DMA (continuous,
re-triggering transfers with no per-block driver intervention) but
`Dma8237` didn't model the reload that makes auto-init actually
auto-*init*. The header's own suggested workaround was to have the chipset
reload the channel itself at block end -- a per-device patch living outside
the DMA controller. That would have been wrong for two reasons: the reload
is genuine 8237A-5 hardware behavior with nothing device-specific about it
(any future DMA client with the autoinit bit set would need the same fix
re-derived independently), and it would leave `Dma8237::advance()` reporting
a wrapped address to a caller that expects a reloaded one, an observable gap
between the model and a real chip.

The fix instead lives in `Dma8237` itself. Per the Intel 8237A-5 data sheet's
"Autoinitialize" section, a real channel latches whatever address and count
it's programmed with into a second, hidden register pair alongside the
normal (working) ones -- `dma8237.h`'s `Channel` struct gains
`base_address`/`base_count` for exactly this, and `out()`'s existing
address/count-write path mirrors every write into both pairs at once, so the
hidden registers always hold "the last full reload," never anything a
driver has to program separately. `advance()` then checks the mode byte's
autoinit bit (0x10) at terminal count: if set, the working registers reload
from the hidden pair instead of continuing to increment/wrap; if clear,
behavior is byte-for-byte what it was before this change. The floppy
controller, `Dma8237`'s only other client, never sets that bit, so this is
purely additive -- confirmed by rerunning the full native suite (475 cases)
with no floppy-side regressions, plus two new `dma8237_test.cpp` cases
(`AutoinitializeReloadsAddressAndCountAtTerminalCount`,
`WithoutAutoinitializeAddressWrapsInsteadOfReloading`) that pin both paths
directly. `chipset.cpp`'s `service_sb_dma()` now just calls `advance()` and
trusts the result, with no autoinit-specific logic of its own.

### 12.3 `service_sb_dma()`: one DMA client, two different channel widths

`service_sb_dma()` moves one block once the device's own real-time pacing
says a transfer is ready (`sb.transfer_ready()`), the same "one bulk copy
per completed real-time wait" shape the floppy's own DMA handoff already
uses -- this is a batched copy standing in for what real hardware does one
byte at a time on every DMA request cycle, not a timing shortcut, since the
device-side pacing (not the copy) is what determines when bytes are allowed
to move at all. It branches on `sb.transfer_is_16bit()` and applies the two
corrections §11.5 documents: the 8-bit path reads/writes `dma1` byte-for-
byte at `(dma1.page(1) << 16) | dma1.address(1)`; the 16-bit path reads
`dma2`'s channel 1 (physical DMA channel 5) as *word*-addressed --
`(dma2.page(1) << 16) | (dma2.address(1) << 1)`, `(count + 1) * 2` available
bytes, one `dma2.advance(1)` per byte *pair* -- exactly as the header's
formulas specify. Both paths finish with `sb.finish_transfer(moved)`,
handing control back to the device to decide (per its own auto-init/
single-cycle state) whether to re-arm for another block or raise its
interrupt and stop.

### 12.4 Exposing both to the browser

`wasm_machine.cpp` adds `injectMouseEvent(dx, dy, buttons)` (a direct
forward to the new chipset method) and `sbDrainSamples()`/`sbSampleRateHz()`
(mirroring the existing `speakerEdges()` shape: drain the device's own
timestamped sample log into typed arrays every call, so the log can't grow
unbounded even while muted). `app.js` gates SB16 output behind the same
"Enable sound" checkbox as the PC speaker -- both are blocked by the same
browser autoplay policy until the user opts in, so one control for both
avoids asking twice -- and adds a separate, also-off-by-default "Enable
mouse" checkbox that requests Pointer Lock on the canvas when checked,
translating `movementX`/`movementY` into PS/2-convention relative motion
(`dy` negated, since the AUX port's own axis convention is +Y-away-from-
the-user, the opposite of a browser's +Y-down-the-screen). Both new audio
paths (speaker and SB16) reuse the existing ring-buffer-behind-an-
AudioWorklet pattern from §8's frame-loop fix -- a worklet's `process()`
callback runs on the real-time audio thread regardless of main-thread
jank, so a brief stall holds the last sample instead of clicking.

## 13. Milestone 4: holding the mouse and the Sound Blaster to BOOM

§10, §11 and §12 built a PS/2 mouse, a Sound Blaster 16 and the chipset
wiring for both, each against its own suite. This section is the milestone's
actual bar, which none of that had yet been held to: **BOOM has to see the
mouse, and BOOM's sound effects have to come out of the DAC.** §10.6 said so
in as many words -- "the bar this milestone is *not* yet at is the same one
every milestone here is judged by: real third-party software".

Neither worked. Four separate defects sat between the two devices and the
software, and **not one of them was in `i8042.cpp` or `soundblaster.cpp`** --
both devices were already right. Two were in the layer that tells DOS what
hardware exists, one was a missing piece of the 8259A, and one was a
two-line direction inversion in the DMA byte mover. They are worth reading
in order, because each was only reachable after the one before it was fixed.

### 13.1 The CPU could not identify itself, so DOS loaded no mouse driver

The starting symptom was as blunt as it gets: with BOOM at its attract
screen, `Chipset::inject_mouse_event()` produced **zero** entries to the
firmware's INT 74h handler, and `I8042::mouse_reporting_enabled()` was
false. Nothing had ever sent the mouse its `F4h` Enable Data Reporting, so
the device was doing exactly what §10.3 says it should: sitting inert until
a driver wakes it.

The driver is supposed to be `CTMOUSE`, which `FDAUTO.BAT` runs. Reading the
live boot instead of the batch file showed it never ran: `MEM /C /N` listed
only `SYSTEM`, `HIMEMX` and `COMMAND`, and the INT 33h vector still pointed
at DOS's own unhandled-interrupt stub (`00D9:1285` -- the same stub §9.2's
IVT dump recorded). The give-away was in the boot transcript itself: the
`MEM /C /N` output appeared **twice**, and there is exactly one path through
`FDAUTO.BAT` that runs `MEM` twice -- `:Only8086`, which falls through to
`:FINAL` and runs it again. The machine was being treated as an 8086.

`FDAUTO.BAT` decides that with FreeDOS's own CPU-identification tool:

```
if not exist %dosdir%\bin\vinfo.com goto Only8086
vinfo /m
if errorlevel 3 goto Support386
if errorlevel 2 goto Support286
```

`vinfo /m` returned **1**. The whole 386+ branch -- `FDAPM`, `CTMOUSE`, and
the CD-ROM setup -- was being skipped on a 486.

`VINFO.COM` is UPX-packed, so the ladder was read out of this machine's own
instruction trace, catching its `INT 21h/AH=4Ch` exit and disassembling the
unpacked image in memory:

```
D3 E8              SHR AX,CL        ; CL=32: unmasked on an 8086, masked to 0 on a 186+
74/75 ...          -> AL=0 (8086)
54 5B 39 D8        PUSH SP/POP BX   ; 286+ pushes the pre-decrement value
                   -> AL=1 (186)
9C B8 00 70 50 9D  POPF 7000h       ; can FLAGS bits 12-14 be set in real mode?
                   -> AL=2 (286)
66 0D 00 00 04 00  OR EAX,40000h    ; AC (bit 18): the documented 386-vs-486 test
                   -> AL=3 (386)
66 B9 00 00 20 00  MOV ECX,200000h  ; ID (bit 21): does POPFD keep it?
                   -> AL=1           ; ...if not
66 B8 01 00 00 00  MOV EAX,1
0F A2              CPUID
C1 E8 08 / 83 E0 0F  SHR AX,8 / AND AX,0Fh   ; AL = family
```

This core passed the first four rungs exactly as a 486 should -- including
AC, which §4's own notes call out as *the* pre-CPUID way to tell a 386 from
a 486 -- and then fell off the last one, because `cpu80486.cpp` deliberately
had no CPUID and `kPopfdMask` deliberately did not let EFLAGS bit 21 stick.
The header comment said why: AP-485 notes that "older versions of Intel486
SX, Intel486 DX and IntelDX2 processors do not support the CPUID
instruction", so an early IntelDX2 genuinely faults on `0F A2`.

**That reasoning was right about the part and wrong about the machine.**
CPUID arrived on the *SL-Enhanced* 486 line -- AP-485 lists IntelDX4 and the
SL-Enhanced IntelDX2 / Intel486 SX / Intel486 DX as having it -- and that is
the part a 1993-94 "maxed out" build ships, not a first-year DX2. Choosing
the earlier stepping is not a neutral detail: it costs this machine its
mouse driver, its APM support and its CD-ROM driver, because the OS it
ships with asks the CPU who it is and believes the answer.

So this core is now the part the rest of the machine claims it is:

- `FLAG_ID` (bit 21) is real storage and `kPopfdMask`/`kIretdMask` let it
  round-trip, which is what makes AP-485's own detection sequence work.
- `0F A2` implements CPUID. Function 0 returns a maximum input value of 1
  (function 2's cache descriptors are Pentium-era) and `GenuineIntel` in
  EBX:EDX:ECX. Function 1 returns `0000_0433h` -- type 0, family 4, model 3,
  which is AP-485's model number for the IntelDX2 -- and feature flags
  `0000_0001h`: the on-die FPU, and nothing else, because every other EDX
  bit names a Pentium-or-later feature.

`vinfo /m` now answers 4, `FDAUTO.BAT` takes `:Support386` -> `:Support386Low`
(the Safe Mode path this machine boots, §5.9), and the boot ends with

```
  SYSTEM      68,336   (67K)     HIMEMX       2,192    (2K)
  COMMAND      3,376    (3K)     FDAPM          928    (1K)
  CTMOUSE      3,104    (3K)     UDVD2        1,984    (2K)
  SHSUCDX      6,224    (6K)     Free       567,696  (554K)
```

-- the machine finally booting its own OS the way that OS intends on a 486,
CD-ROM driver included ("CD-ROM configured as D: drive").

### 13.2 CuteMouse asks the *BIOS* whether a mouse exists

With `CTMOUSE` finally running it printed:

```
CuteMouse v2.1 beta4 [FreeDOS]
Error: device not found
```

Tracing it to its exit and disassembling the unpacked image found the gate
in three instructions:

```
0EA7  CD 11        INT 11h          ; BIOS equipment list
0EA9  A8 04        TEST AL,4        ; bit 2: pointing device installed
0EAB  74 F7        JZ  give-up      ; -> "device not found"
0EAD  B7 03 / B8 05 C2 / CD 15      ; only then: INT 15h AH=C2h AL=05, initialize
```

The driver never touches the 8042 until the firmware says a mouse is
installed. The BDA equipment word at 0x410 read `0021h` -- bit 2 clear --
because `Machine::configure_factory_cmos()` seeded CMOS byte 0x14 with
`0x01`, floppy-installed and nothing else, and this BIOS copies that byte
into the equipment word.

This is the same shape as §5.3's extended-memory finding: a CMOS field left
unasserted because nothing had yet been observed to read it, and real
software that gates a whole feature on it. The machine genuinely has a mouse
on the 8042's AUX port, so its CMOS now says so -- byte 0x14 becomes `0x05`,
floppy plus pointing device, exactly what a real board's Setup/POST records
for a machine with a mouse in the round DIN. `CTMOUSE` then prints
**"Installed at PS/2 port"**, hooks INT 33h, and `mouse_reporting_enabled()`
goes true.

### 13.3 The 8259A had no priority resolver, and every mouse packet arrived scrambled

With a driver installed, injected motion started producing INT 74h entries
-- three per packet, one per byte, exactly as §10.3 predicts -- and Allegro's
INT 33h callback started firing. But **button presses did nothing at all**:
BOOM ignored them, and a click that should have popped up its menu produced
no INT 33h traffic whatsoever, while motion did.

Following the guest from the IRQ12 handler entry showed what the firmware
was actually handing the driver. The far call into `CTMOUSE` carries the
packet on the stack -- status, X, Y from the lowest address up, per the PS/2
BIOS interface -- and the driver reads them at `[BP+1E]`, `[BP+1C]`,
`[BP+1A]`. For an injected left-click (`09 00 00`: bit 3 always set, left
button down, no movement) it read **status 0, X 0, Y 9**. The status byte
had landed in the Y slot. The button was being delivered as movement, which
is why motion "worked" and clicks did not: garbage that looks like movement
is still movement.

The instruction trace showed why, plainly:

```
F000:5A76  MOV DX,60h
F000:04F7  IN  AL,DX        <- the handler reads packet byte 1 (09h)
0070:00BF  ...              <- IRQ12 again, *immediately*, before it stores it
```

The firmware's INT 74h handler opens with `STI` (it is a `sti / pusha /
push ds ...` wrapper around `int74_function`), and `I8042` re-asserts IRQ12
inside the very `in(0x60)` that cleared it, because the next packet byte is
already queued -- the behavior §12.1 deliberately models as a level check
rather than an edge. So the handler was re-entered between reading a byte
and storing it into the EBDA, three deep, and the three bytes were written
into the packet buffer in the reverse of the order they arrived.

**On real hardware this cannot happen, and the reason is the 8259A.** When
the chip acknowledges an interrupt it sets that line's In-Service bit, and
its priority resolver then refuses to assert INT for anything of *equal or
lower* priority until an EOI clears it -- Intel's data sheet calls this
fully nested mode: "interrupts are ... allowed only if they are of higher
priority than the one currently being serviced". A handler cannot be
re-entered by its own device. That is precisely why the firmware can afford
to `STI` on entry.

`pic8259.cpp` set the ISR bit on `acknowledge()` and then never read it. The
header comment claimed the behavior -- "sets its ISR bit so a lower-priority
interrupt can't preempt it until EOI'd" -- and `unmasked_pending()` computed
`irr_ & ~imr_`, with no ISR term at all. An existing test even asserted the
bug (`AcknowledgeClearsIrrAndSetsIsrUntilEoi` expected a lower-priority line
to be served while a higher one was in service, with a comment explaining
that `has_interrupt()` reflects only IRR & ~IMR).

The fix is the resolver, in one function:

```cpp
uint8_t in_service_mask() const {
    if (isr_ == 0) return 0xFF;
    return uint8_t((1u << __builtin_ctz(isr_)) - 1u);   // strictly higher priority only
}
uint8_t unmasked_pending() const { return uint8_t(irr_ & ~imr_ & in_service_mask()); }
```

Fixed priority means the highest-priority in-service line is the lowest set
ISR bit, and only requests strictly above it pass. Equal priority -- the
line's own bit -- is blocked, which is the whole point. With that in place
the same click delivers `status 09h, X 0, Y 0`, and `CTMOUSE` calls
Allegro's callback with **AX=0002h** (left button pressed) and **BX=0001h**
(left button down), which is what BOOM had been waiting for.

This was never a mouse bug. It is a core interrupt-controller behavior that
every device on the machine has been running without; the mouse is simply
the first device here whose line re-asserts *inside its own handler*, which
is the one case where the missing block is fatal rather than merely
untidy. The floppy, the IDE channels and the keyboard all raise one
interrupt and wait to be drained, so none of them ever noticed.

### 13.4 The Sound Blaster's DMA ran backwards

With the mouse working, phase 3's audio check reported a stream that was
running perfectly and carrying nothing: 222,824 samples at exactly the
22,727 Hz the driver programmed, IRQ5 firing 434 times, and **every single
sample identical, at -32768**.

A constant is not audio, but it is also not silence, and the specific
constant is the evidence. Allegro programs this card with `B6h` + mode
`20h`: 16-bit **unsigned** stereo, auto-init, 512-frame blocks. Unsigned
silence is `8000h`; the value the DAC was latching was `0000h`, which
`expand16` centers to exactly -32768. The card was playing a buffer full of
zeros -- and a 4KB checksum of the guest's DMA buffer, polled across the
run, was *changing constantly*, so Allegro was mixing real audio into it the
whole time.

`Chipset::service_sb_dma()` had the two directions swapped:

```cpp
if (sb.transfer_is_input()) { for (...) buf[i] = mem[...]; }   // WRONG
else                        { for (...) mem[...] = buf[i]; }   // WRONG
```

`transfer_is_input()` is the DSP command's A/D bit: *input* means recording,
device -> memory. Playback is the other direction. The shape was borrowed
from the floppy's handoff immediately above it, where `fdc.transfer_is_write()`
means write-to-*disk* and therefore memory -> image -- the opposite sense of
the same-looking flag. The result was that every playback block copied the
card's own (zero-initialised) buffer *out over* Allegro's mixed audio, and
then `finish_transfer()` turned that same buffer into samples. Both halves of
the bug hid each other: the DAC output was consistent with what the card
believed it had fetched, and the guest's buffer was being corrupted by the
emulator at exactly the rate the guest was refilling it.

Swapping the two branches is the whole fix. The same run now produces
263,217 non-silent samples spanning the full ±32767 range across 2,634
distinct values; sampled over a two-second window at the first sound, the
waveform has an RMS of 2,793 (about 8.5% of full scale, consistent with
BOOM's `sfx_volume 8`), roughly 2,100 zero-crossings per second -- effect-
shaped, nowhere near noise's ~11 kHz -- and no clipping at all.

### 13.5 What `make boom-check` now proves

`disks/boom_run_check.cpp` grows from one phase to three, all of which must
pass. Phase 1 is §9.6's renderer bar, unchanged.

**Phase 2, the mouse.** Three facts, measured on screen:

- A **left click during the attract demo brings up BOOM's menu**, with no
  keyboard anywhere in the path: DOOM's own `G_Responder` treats any mouse
  button during demo playback like a keypress and calls
  `M_StartControlPanel`. The menu is a static overlay on a 3D view that
  repaints every frame, so the measurement is "how much of the view stopped
  changing" -- 8,824 pixels of 53,760 holding still across four frames
  before the click, 11,553 after -- and it needs to know nothing about where
  BOOM draws its menu.
- **Motion turns the player's view, by the amount injected.** Walking that
  menu into a live game is deliberately keyboard, so the mouse is the only
  thing under test; a standing player's view is then perfectly still (**0**
  changed pixels over 4.5 seconds). Six `dx=+80` events change **49,336** of
  53,760 view pixels, and six `dx=-80` events bring it back to a frame that
  is **pixel-identical** to the one it started from (**0** differences). A
  view that turns and returns exactly is not a screen that happens to be
  moving: it is BOOM adding up the same movement counts twice, with opposite
  signs.
- **Button 1 fires.** `BOOM.CFG`'s `mouseb_fire 0` makes the left button the
  fire key; a press against that still view changes up to **44,243** pixels
  (muzzle flash and the firing frame of the weapon) and the captured frame
  shows the AMMO counter down from 50.

**Phase 3, the sound.** Judged over the whole run, with bars written so the
bug that was actually here would fail them: a constant fails (at least 256
distinct sample values), a DC level fails, a stream that never rises above
the noise floor fails (peak amplitude at least 4096), a silent stream fails,
IRQ5 never firing fails, and playing *faster* than the programmed rate fails
-- the same CLAUDE.md-mandated realism guard `PlaybackIsPacedAtTheRealSample
RateAndNeverFaster` puts on the device, measured here against real
third-party software's own rate. The run reports 1,183,155 samples over
52.06 s of emulated time = 22,727 Hz against a programmed 22,727 Hz, ratio
1.000.

One new flag, `--sb-trace`, logs every DSP state change (rate, width,
channels, speaker gate) as it happens; the header comment documents it with
the rest.

### 13.6 What this cost, and one invariant that legitimately moved

- `hdd_boot_check` reaches its idle `C:\>` at cycle **1,645,095,079**,
  against the **1,409,073,889** §8.3 and §9.6 both recorded as a
  cycle-for-cycle invariant. That figure moved for a real reason and is
  expected to stay moved: the boot now runs `FDAPM`, `CTMOUSE`, `UDVD2` and
  `SHSUCDX`, which it never did before (§13.1). Conventional memory free
  drops from 566K to 554K for the same reason. Nothing about real mode's
  own timing changed -- the identical-cycle check is simply no longer
  measuring the identical boot.
- `make boom-check` goes from about 65 s to about 100 s of wall clock,
  all of it phase 2 playing BOOM. No clock was sped up to pay for it; this
  harness still has no equivalent of the `fast=1` flag CLAUDE.md sanctions
  for the browser suites, and it should not acquire one.

### 13.7 Tests

`make check`: **487 GoogleTest cases pass** (477 before), `pm-check` 24/24,
`vbe-check` all-pass, `hdd-boot-check` and `boom-check` green.

- `pic8259_test.cpp` gains `ALineInServiceCannotInterruptItselfUntilEoi`
  (the mouse case, stated as the chip behavior it actually is),
  `HigherPriorityLinePreemptsOneInService` (the other half of fully nested
  mode -- the timer must still tick through a slow handler),
  `AutoEoiSetsNoInServiceBitSoNothingIsBlocked`, and
  `SpecificEoiUnblocksOnlyTheNamedLine`.
  `AcknowledgeClearsIrrAndSetsIsrUntilEoi` is rewritten: it asserted the
  bug.
- `chipset_test.cpp` gains
  `MousePacketBytesArriveOneInterruptAtATimeInOrder` (the whole §13.3 story
  end to end: three interrupts, correct AUXB tagging, correct byte order,
  and no delivery until EOI),
  `SoundBlasterPlaybackDmaReadsMemoryIntoTheCard` (which also asserts that
  playback leaves memory untouched -- the half of §13.4 that was corrupting
  the guest),
  `SoundBlasterRecordingDmaWritesTheCardsSamplesIntoMemory`, and
  `SoundBlasterBlockEndRaisesIrq5`.
- `cpu80486_test.cpp`'s `CpuidIsAbsentOnAnEarlyDx2AndFiresTheDiagnosticHook`
  is replaced by `CpuidFunctionZeroReportsGenuineIntelAndAMaximumInputOfOne`,
  `CpuidFunctionOneReportsAnIntelDx2SignatureWithOnlyTheFpuFeature` and
  `TheIdFlagRoundTripsSoSoftwareCanDetectCpuid` (AP-485's own detection
  sequence, which is the one FreeDOS runs). `PopfdDoesNotSetReservedOrVirtual
  ModeBits` now excepts bit 21 and asserts it explicitly.
- `machine_test.cpp` asserts CMOS 0x14 bit 2, with CuteMouse's `INT 11h /
  TEST AL,4` as the reason.

### 13.8 What this says about where bugs live

§9.9 made the case that only real third-party software samples the
instruction set the way period compilers emitted it. This section makes the
same case one layer out, and more sharply: **the two devices this milestone
built were both correct, and all four defects were in what surrounds them.**
Two devices, 37 + 41 unit tests between them, every register and response
byte checked against Creative's and Chapweske's own documents -- and the
mouse was inert because a CMOS bit was clear and the CPU would not say what
it was, the packets were scrambled by a missing line in the interrupt
controller, and the audio was inaudible because two `for` loops assigned in
the wrong direction. None of those is a device bug, and no device-level
suite could have found any of them.

The pattern is worth naming, because it is the same one every time: a unit
test proves a component answers correctly when asked correctly. Only real
software proves anything about who does the asking -- whether the OS ever
loads the driver, whether the driver believes the hardware is there, whether
the interrupt survives the trip, whether the bytes go the way they are
supposed to go.

### 13.9 Sources

- **Intel 8259A data sheet, "Fully Nested Mode" and "Priority Resolver"** --
  the in-service block behind §13.3: a request is granted only when it
  outranks everything currently in service, with equal priority blocked, and
  EOI as the only thing that lifts it.
- **Intel AP-485, "Intel Processor Identification and the CPUID
  Instruction"** -- which Intel486 parts carry CPUID (IntelDX4 and the
  SL-Enhanced IntelDX2/486 SX/486 DX; the older parts fault), the EFLAGS.ID
  toggle as the documented way to detect it, the vendor-string layout, and
  the family/model table that makes family 4 model 3 an IntelDX2.
- **FreeDOS 1.3's own `VINFO.COM` and `FDAUTO.BAT`**, as shipped on this
  machine's disk image -- cited as the primary evidence for §13.1: the
  CPU-generation ladder disassembled in §13.1 is read straight out of this
  machine's instruction trace, and `FDAUTO.BAT`'s `if errorlevel 3 goto
  Support386` is what makes the answer decide whether the machine gets a
  mouse driver at all.
- **CuteMouse 2.1 beta4 (`CTMOUSE.EXE`)**, likewise from the shipped image
  -- the `INT 11h / TEST AL,4` gate in §13.2 and the INT 15h AH=C2h sequence
  behind it, read from this machine's own trace.
- **Bochs BIOS `rombios.c`** (the ROM this machine boots) -- `int74_function`
  and its `sti`-on-entry assembly wrapper, the EBDA packet buffer it
  assembles into, and the far call whose stack layout (status, X, Y, Z) is
  what §13.3 reads the scrambling off. Cited for what the firmware
  *requires* of the hardware, per §7.11's rule.
- **Creative, *Sound Blaster Series Hardware Programming Guide*** -- the
  `Bxh` command and mode-byte bit fields (§11.6's citation), which is what
  says mode `20h` is 16-bit *unsigned* stereo and therefore what makes
  `0000h` in the buffer full-scale negative rather than silence.
- **Allegro 3.x `sb.c` and its DOS mouse driver** -- what BOOM's driver
  actually issues: `B6h`/mode `20h` at 22,727 Hz in 512-frame auto-init
  blocks on the 16-bit channel, and the INT 33h function 0Ch callback
  (registered with CX=007Fh, all events) through which button and motion
  events reach the game.
- **DOOM/BOOM `g_game.c` and `m_menu.c`** -- `G_Responder`'s demo-abort
  branch (`ev->type == ev_mouse && ev->data1` pops up the menu), which is
  what phase 2's click test relies on, and `BOOM.CFG`'s `use_mouse 1` /
  `mouseb_fire 0` settings the shipped image carries.

## 14. BOOM runs at 60% speed, and it is not the chipset's fault

Reported symptom: BOOM is noticeably slow in the browser, on more than one
host. §8 left this machine with 76.5 M cycles/sec against a 66.0 MHz
requirement, and §12/§13 then added per-instruction work to
`Chipset::tick()` (the Sound Blaster's tick and DMA service, the mouse's
IRQ12 level check, IRQ5's edge check) and a priority resolver to the 8259A
that `has_interrupt()` now runs twice per instruction. The obvious
hypothesis was that those additions had eaten the headroom.

**They had not.** Measured, not assumed, and the hypothesis is worth
recording as wrong because the real answer is somewhere else entirely.

### 14.1 §8's 76.5 M/s never described this workload

§8.1/§8.3 benchmarked `runCycles(20_000_000)` "at an idle FreeDOS `C:\>`
prompt". Two things make that number inapplicable to BOOM:

- It is **real mode with paging off**, where `seg_linear()` is one add and
  `translate()` returns its argument. BOOM is 32-bit protected mode under
  CWSDPMI with paging enabled, so every guest byte pays a segment limit
  check and a page translation.
- Since §13.1 the boot loads `FDAPM`, which halts the CPU when DOS is idle.
  The same benchmark at the same prompt now reports **394 M cycles/sec**,
  which is not interpreter throughput at all -- it is `Cpu::step()`'s
  `if (halted) { cycles += 4; return 4; }` measured 5 million times. The
  idle prompt has stopped being a usable benchmark.

Re-measured where the complaint actually lives -- the same
`runCycles(20_000_000)` timing loop, taken with BOOM in mode 13h at its
attract demo, real-speed page (`?test=1`, no `fast=1`):

```
36.1  36.3  36.9  37.0  38.9   M cycles/sec
```

Against 66.0 M/s required. The pump (§8.4) then does exactly what it was
built to do -- sheds the excess rather than blocking -- so BOOM runs at
about 60% of real speed instead of freezing the tab. `smoke.spec.ts`'s
33-99 M/s band covers this, for the same reason §8.7 gives: the band is
wide and the test measures a real-mode boot, not a game.

### 14.2 Where the cycles go, from two profilers

Native (`sample`, `-O2 -g`, `disks/boom_run_check` during phase 2, 25 s,
top of stack) and in the browser (Chrome's sampling profiler over a CDP
session, against the shipped `-O3 -flto` wasm rebuilt with
`--profiling-funcs` so the name section survives, 10 s under BOOM). The two
agree:

| wasm, self time                    | share |
| ---------------------------------- | ----- |
| `runCycles` (everything LTO inlined into it) | 43.9% |
| `Cpu::translate`                   | 16.5% |
| `Cpu::decode_modrm`                | 7.7%  |
| the `Bus` memory-read thunk        | 6.7%  |
| `Cpu::seg_linear`                  | 6.7%  |
| `Cpu::read32`                      | 5.9%  |
| `Cpu::write32` + the write thunk   | 3.2%  |

**About 39% of the whole run was the logical -> linear -> physical path**,
and the native profile (which does not inline across translation units, so
it can attribute what wasm folds into `runCycles`) puts the chipset at
roughly 19%: `Chipset::tick` 8.9%, `Pit8253::tick` 3.6%, `SoundBlaster::
advance` 2.6%, `Chipset::service_sb_dma` 1.8%, `PcSpeaker::update` 1.7%.

That 19% is the answer to the opening hypothesis. Everything §12 and §13
added -- the SB tick, the DMA service call, the mouse and IRQ5 checks -- is
inside the Sound Blaster's 2.6% plus the DMA service's 1.8%, and both are
only non-zero because BOOM is playing audio. The 8259A's new
`in_service_mask()` does not appear in either profile at all; it is a
compare, a `__builtin_ctz` and a subtract, inlined into `Machine::
run_cycles`, whose entire self time is 2.7% including the run loop itself.
**Milestone 4's chipset work costs on the order of 5%, not 45%.**

The build flags were checked too, since §8.3 had changed them: `web/
Makefile`'s `EMFLAGS` still carries `-O3 -flto -fwasm-exceptions
-sSUPPORT_LONGJMP=wasm` verbatim. No regression there.

### 14.3 The real finding: one segment check and one page walk *per byte*

`read16`/`read32`/`read64` were composed out of `read8`, and `write16`/
`write32`/`write64` out of per-byte `seg_linear()` + `translate()` pairs. A
32-bit read therefore ran **four** full segment-limit checks and **four**
page translations; a 64-bit FPU load ran eight of each. In real mode both
are nearly free, which is why §8 never saw it. Under BOOM they are the
single largest cost in the machine.

A real 486 does neither. Intel's 80486 PRM has the segmentation unit check
the whole access against the limit before any bus cycle is run, and the
paging unit consult the TLB once per page touched -- not once per byte.

Three changes, all of which leave *what* the machine does alone:

- **`Cpu::access_phys()`** resolves a whole `size`-byte access with one
  `seg_linear()` and one `translate()` when the access is a contiguous run
  of offsets inside a single page. It refuses -- and the old byte-at-a-time
  path runs unchanged -- when the run wraps real mode's 64KB boundary,
  overflows 32 bits, or straddles a page boundary, which is what keeps a
  straddling access faulting exactly where it did. `read16/32/64` and
  `write16/32/64` try it first. The one behaviour this does change is the
  narrow case of an access whose *first* byte is inside the segment limit
  and whose last is not: it now raises #GP before reading byte 0 rather
  than after, which is what the PRM says the segmentation unit does.
- **`seg_linear()` and `translate()` are split into an inline fast path and
  an out-of-line `_slow()`.** The fast paths cover the ordinary present,
  expand-up, correctly-typed segment and the TLB hit; `_slow()` is the
  complete original function, entered in exactly the cases the fast path
  declines, so a stale TLB entry still faults off its own cached rights
  rather than off a fresh walk. Same §8.3 playbook as the device ticks.
- **Three more per-instruction calls made cheap the same way.**
  `PcSpeaker::update()` (an AND gate and a compare that changes nothing on
  a silent machine) and `Pit8253::tick()`'s credit accumulation move into
  their headers, with the PIT's channel stepping left out of line behind an
  `if (whole == 0) return 0;` -- at 66 MHz the PIT advances one count every
  ~55 CPU cycles, so almost every call ends there. `Chipset::tick()` now
  guards `service_sb_dma()` behind the device's own inline
  `sb.transfer_ready()` flag, the same shape as `run_cycles()`'s
  `has_interrupt()` guard.

Measured, same benchmark as §14.1:

|                                  | before        | after          |
| -------------------------------- | ------------- | -------------- |
| wasm, BOOM at its attract demo   | 36.1-38.9 M/s | **38.9-41.4 M/s** |
| wasm, halted at the idle prompt  | 389-394 M/s   | **477-481 M/s**   |

In the re-profile `translate` and `seg_linear` no longer appear as frames at
all -- their fast paths fold into `read8`/`read32`/`access_phys` -- so the
share table is no longer comparable line for line, and the throughput figures
above are the honest measure of the change.

### 14.4 What this does not fix, and the honest number

**BOOM still does not reach 66 MHz on this host.** ~40 M/s against 66.0 is
about 60% of real speed, and §8.4's pump means the visible result is a game
running slow rather than a tab that stops answering. That is an improvement
of roughly 8% on a shortfall of 65%, and it would be dishonest to call it a
fix for the reported symptom.

What the profiling establishes is where the remaining work is, none of it
in the chipset:

- **The `Bus` memory thunks, 8-10%.** Every guest byte crosses
  `Chipset::mem_read`/`mem_write`, which re-checks the A20 gate, the VGA
  window, the RAM bound and (for writes) a bit-packed `vector<bool>` ROM
  map. Those four checks are per *byte*; they are properties of a 4KB
  physical page. A page-resolution cache in the CPU -- physical page ->
  host pointer plus writability, filled by one chipset query -- would
  collapse them, and is the largest single win left.
- **Instruction fetch.** `fetch8`/`fetch16`/`fetch32` take the full
  logical -> physical path for every opcode and displacement byte, so a
  five-byte instruction pays five translations before it executes. Real
  silicon has a prefetch queue and a separate code TLB.
- **`decode_modrm` at 5-8%**, and `step_inner`'s switch dispatch inside the
  `runCycles` blob.

None of these is a bug; they are the cost of a byte-at-a-time interpreter
meeting 32-bit paged protected-mode code for the first time. The point of
recording them here with numbers attached is that the next pass at this
should start from the memory path, not from the chipset -- which is exactly
the mistake this section started by making.

### 14.5 Tests

`make check` green: **488 GoogleTest cases** (487 before), `pm-check` 24/24,
`vbe-check` all-pass. The new case is `cpu80486_test.cpp`'s
`AnAccessStraddlingTheLimitFaultsBeforeMovingAnyByte`, which pins the one
behaviour §14.3 changed: a write whose first bytes are inside the segment
limit and whose last are not now raises #GP with memory untouched. `make boom-check` green on all three phases, and its
figures are **identical to the ones §13.5 recorded** -- 8,824 -> 11,553
still pixels around the menu click, 49,336 view pixels turned and 0 on the
way back, 44,243 on the shot, 1,183,155 samples at 22,727 Hz against a
programmed 22,727 Hz. A pixel-for-pixel and sample-for-sample identical
BOOM run is the strongest available statement that none of this changed
what the machine does.

## 15. The bus stops being a per-byte question, and so does instruction fetch

§14.4 left three named targets and a ranking: the `Bus` thunks first, then
instruction fetch, then `decode_modrm`. Profiled on this build before
touching anything, **the ranking is wrong** -- the thunks and fetch are the
same cost, because almost every byte crossing a thunk *was* an instruction
byte.

### 15.1 What the profiler showed

`sample` over `disks/boom_run_check`'s workload (`-O2 -g`, BOOM in mode 13h
at its attract demo, 25 s, ~19,300 samples, top of stack):

| self time                          | share |
| ---------------------------------- | ----- |
| `Cpu::read8`                       | 18.3% |
| `Chipset::tick`                    | 14.9% |
| `Cpu::step_inner`                  | 13.3% |
| `Cpu::decode_modrm`                | 9.2%  |
| the `Bus` memory-read thunk        | 7.9%  |
| `Cpu::access_phys`                 | 6.9%  |
| `SoundBlaster::advance`            | 3.4%  |
| `Cpu::read32`                      | 2.9%  |

Attributing `read8` to its callers is what settles the ranking: 1,689
samples from `step_inner`, 1,331 from `decode_modrm`, 269 from `two_byte`,
94 from `grp1_immed`, 50 from `grp2_shift` -- **97% of it is `fetch8`**, and
`rm_read8` is 17 samples. The same holds a level down: of the read thunk's
samples, 416 come from `decode_modrm` and 375 from `step_inner`. A five-byte
instruction was paying five segment checks, five page translations and five
indirect calls into the chipset *before it executed*, and the thunk cost
§14.4 measured was mostly being paid on its behalf.

### 15.2 A physical page resolves once, not once per byte

`Chipset::mem_read`/`mem_write` re-derive four things per byte -- the A20
gate, the VGA window, the RAM bound, and (for writes) a bit-packed
`vector<bool>` ROM map. All four are properties of a 4KB *page*.

`Chipset::page_host(page_base, write)` answers them once and returns a host
pointer into `mem`, or `nullptr` when the page is not plain memory: the VGA
window, unpopulated space above `mem`, or -- for a write -- a page holding
any ROM byte (a new page-granular `rom_page_` mirror of `rom_`, so a partly
-ROM page is refused rather than scanned). `nullptr` is not a failure; it
sends that access back through the byte-at-a-time thunks, which is exactly
what keeps the VGA window, ROM write-protection and open bus behaving as
before.

The CPU caches the answer in a 64-entry direct-mapped table per direction
and uses it from `read8`/`write8` and from every access `access_phys()`
already resolved. Caching a resolved translation is what the silicon does
too -- this is a step below the TLB §14 left alone, not a shortcut past it.

**Invalidation is the whole correctness question**, and it has one real
trigger: A20. `Chipset` keeps a generation counter the CPU reads (one L1
load per access) and bumps it whenever an already-resolved page could now
resolve differently -- the gate moving, a `load_rom`, a reset. The gate is
the 8042's output-port bit 1 and nothing else on this machine, so it is
checked after every write to the controller (where a guest moves it) and
once per `tick()` (so a host poking `kbc` directly is caught too). VGA bank
switching needs no trigger: the window is never handed over in the first
place.

The `Bus` gains the pair as *optional* members, bound by `Bus::For` only for
a host that offers them (`if constexpr` on a detection trait). The three
test fixtures that build a `Bus` over a bare byte array keep the original
path, unchanged.

### 15.3 Instruction fetch gets a prefetch window

With the thunks gone, fetch still paid a limit check and a page walk per
byte. `prefetch_fill()` resolves the run of EIPs sharing one code page once,
and `fetch8`/`fetch16`/`fetch32` read the rest out of a host pointer. It
declines -- leaving an empty window, so every fetch takes the original path
-- for a run that crosses a page, passes the segment limit or real mode's
64KB wrap, or sits on a page the bus will not hand over. A real 486 has a
prefetch queue and does the same thing for the same reason.

The first version cleared the window at the top of every instruction, which
is trivially safe and left `prefetch_fill` at 12% of the run: one refill per
instruction rather than one per byte, a real gain but a third of the
available one. The window now survives across instructions and is
re-validated once per instruction against every piece of state its EIP ->
host-pointer mapping rests on: CS's cached descriptor (base, limit, access,
D/B, null), CR0, CR3, CPL, a counter every TLB flush and INVLPG bumps, and
the bus generation above. Comparing those *values* -- rather than clearing
the window at each site that might write them -- is what makes it safe
without an enumeration to get wrong, and it rests on the same architectural
contract the TLB already does: software that changes a page mapping issues
an INVLPG or reloads CR3. Nothing mid-instruction can move any of them,
because a CS load, a CR3 write or a mode change ends the instruction it
happens in. And because the window holds a pointer into live memory rather
than a copy, code that patches itself ahead of EIP still executes the
patched byte.

### 15.4 The numbers

Same benchmark as §14.1 (`Machine::total_cycles()` over a real-time window,
BOOM at its attract demo), native, built with the shipped module's `-O3
-flto` so the inlining matches; baseline is this same source with
`Bus::For`'s page binding disabled, so both sides are one build apart:

|                     | before        | after             |
| ------------------- | ------------- | ----------------- |
| BOOM, attract demo  | 58.9-69.2 M/s | **86.2-97.8 M/s** |
| mean                | 62.3 M/s      | **90.0 M/s**      |

**+44%.** These are native figures, not the browser's -- §14.1's 36-41 M/s
were measured in wasm, and the wasm build has to be re-measured in a browser
before any claim about BOOM's actual frame rate is made.

The re-profile says where the interpreter now spends itself: `step_inner`
28%, `Chipset::tick` 22% (inflated natively, where the device ticks do not
inline across translation units the way LTO makes them in the wasm build),
`decode_modrm` 13%, then `Cpu::step` 5%, `SoundBlaster::advance` 4.9%,
`access_phys` 4.8%. `read8` fell from 18.3% to 1.5%, `read32` to 1.2%,
`prefetch_fill` and the `Bus` thunks out of the listing entirely, and
`Chipset::page_host` never appears at all -- the page cache essentially
never misses.

`decode_modrm`, §14.4's third target, was profiled rather than assumed: most
of its apparent cost was the fetching *inside* it (1,331 `read8` and 597
`read32` samples charged to it), which is now gone. What remains is genuine
decode -- the ModR/M and SIB address arithmetic -- and it is third behind
the interpreter's own dispatch. Anything further is a restructuring of the
interpreter, not another cache.

### 15.5 Tests

`make check` green: **495 GoogleTest cases** (488 before), `pm-check` 24/24,
`vbe-check` all-pass. The seven new cases pin the parts that had to keep
working: `page_host()` refusing the VGA window, open bus and a ROM page for
writes while handing over plain RAM; the generation counter moving for a
gate change, a `load_rom`, and a gate change made outside `io_out`; and four
cases that run real guest code through a real `Machine` -- a store to ROM
through an already-resolved page, a store and a load in the VGA window
neither reaching nor reading the RAM behind it, opening A20 mid-run and
watching the next store to the same offset land a megabyte higher, and code
that patches a byte ahead of EIP on its own page and executes the patch.

`make boom-check` green on all three phases, with figures **identical to the
ones §13.5 and §14.5 recorded** -- 8,824 -> 11,553 still pixels around the
menu click, 49,336 view pixels turned and 0 on the way back, 44,243 on the
shot, 1,183,155 samples at 22,727 Hz against a programmed 22,727 Hz. The
same pixel-for-pixel and sample-for-sample identical run §14 relied on, and
the same reason: it is the strongest available statement that none of this
changed what the machine does.
