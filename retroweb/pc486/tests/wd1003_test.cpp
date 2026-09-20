// GoogleTest suite for the WD1003-WA2-compatible hard disk controller on
// this machine's primary IDE channel: the genuine ATA-style detection
// sequence a legacy BIOS actually uses (scratch-register signature, soft
// reset, post-reset signature), IDENTIFY DEVICE geometry, and paced
// READ/WRITE SECTORS via the real atomic 16-bit data register path.
//
// The geometry here is this machine's 504MB drive -- 1024 cyl / 16 head / 63
// sec, the exact pre-EIDE INT 13h CHS ceiling (see wd1003.cpp's mount() and
// PC486_REVIEW.md), not the AT's ST-4038.

#include <gtest/gtest.h>

#include "wd1003.h"

#include <vector>

namespace {

using pc486::Wd1003;

// This machine's fixed geometry.
constexpr int kCylinders = 1024;
constexpr int kHeads = 16;
constexpr int kSectorsPerTrack = 63;
constexpr long kCapacitySectors = long(kCylinders) * kHeads * kSectorsPerTrack;  // 1,032,192

// Deliberately a TRUNCATED prefix of the real 504MB capacity: these tests
// only ever touch low sectors, the controller bounds-checks requests against
// the mounted image's actual size, and materializing 528,482,304 real bytes
// per test case would cost half a gigabyte of test-process memory for
// nothing. IDENTIFY's reported geometry comes from mount()'s fixed constants,
// not from the image length, so the geometry tests below are unaffected.
std::vector<uint8_t> MakeImage(long sectors) {
    std::vector<uint8_t> img(std::size_t(sectors) * 512, 0);
    // Stamp each sector's first byte so a test can confirm exactly which
    // sector's bytes came back.
    for (std::size_t i = 0; i < img.size(); i += 512) img[i] = uint8_t((i / 512) & 0xFF);
    return img;
}

class Wd1003Test : public ::testing::Test {
protected:
    Wd1003 hdd;
    void SetUp() override { hdd.reset(); }

    void MountDrive0(long sectors = 4096) {
        auto img = MakeImage(sectors);
        hdd.mount(0, img.data(), img.size());
    }
};

TEST_F(Wd1003Test, ScratchRegisterSignatureDetection) {
    // The real BIOS detection sequence (confirmed against rombios.c's
    // ata_detect(), see PC486_REVIEW.md): write a scratch pattern to
    // Sector Count/Number and read it back to confirm something answers.
    hdd.out(0x1F6, 0xA0);  // select device 0
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    hdd.out(0x1F2, 0xAA);
    hdd.out(0x1F3, 0x55);
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    EXPECT_EQ(hdd.in(0x1F2), 0x55);
    EXPECT_EQ(hdd.in(0x1F3), 0xAA);
}

TEST_F(Wd1003Test, SoftResetReassertsPostResetSignature) {
    // After scribbling the scratch registers, a soft reset (Device Control
    // SRST bit) must bring back the ATA post-reset signature the real
    // detection code checks next: sector count/number = 1, cylinder = 0,
    // status non-zero. Cylinder 00h/00h is specifically what says "ATA hard
    // disk" rather than "ATAPI packet device" (14h/EBh) -- see
    // atapi_cdrom.h, which implements the other half of that distinction on
    // the secondary channel.
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // release it -- real hardware resets on this edge
    EXPECT_EQ(hdd.in(0x1F2), 0x01);
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);  // cylinder low
    EXPECT_EQ(hdd.in(0x1F5), 0x00);  // cylinder high
    EXPECT_NE(hdd.in(0x3F6) /* alt status, doesn't clear IRQ */, 0x00);
}

TEST_F(Wd1003Test, SoftResetSignalsAbsenceForTheSlaveViaFloatedCylinderRegisters) {
    // The real BIOS's ata_detect() distinguishes "real device" from "no
    // device" purely from what a soft reset leaves in Cylinder Low/High:
    // 0x00/0x00 for a device that's actually there and drives its own
    // signature, 0xFF/0xFF for a position nothing answers from (floated
    // by the bus's pull-ups). Sector Count/Number read back 1/1 either
    // way -- those are shared-bus registers whichever real device is on
    // the cable always drives, present or not (see out()'s comment).
    // Getting the Cylinder Low/High distinction wrong made the
    // permanently-absent slave look like a genuine second drive to
    // ata_detect(), whose inevitably-refused IDENTIFY attempt then made
    // the real BIOS panic with "Failed to detect ATA device" after a
    // real ~32-second hardware timeout waiting for a response that could
    // never come. See PC486_REVIEW.md.
    hdd.out(0x1F6, 0xB0);  // select drive 1 (slave, never mounted)
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // release it
    EXPECT_EQ(hdd.in(0x1F2), 0x01);  // sector count/number still read back 1/1...
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0xFF);  // ...but cylinder low/high floats, signaling absence
    EXPECT_EQ(hdd.in(0x1F5), 0xFF);

    // Selecting drive 0 and resetting again confirms the genuinely-present
    // master still gets its real 0x00/0x00 signature, unaffected by the
    // slave-selected reset that just happened.
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x3F6, 0x04);
    hdd.out(0x3F6, 0x00);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);
    EXPECT_EQ(hdd.in(0x1F5), 0x00);
}

TEST_F(Wd1003Test, FloatedSignatureIsATwoReadWindowNotAPersistentState) {
    // The floating illusion from the test above must NOT outlive the
    // exact two reads (Cylinder Low, then High) the real BIOS's
    // ata_detect() uses it for -- a real, and quite subtle, bug: this
    // BIOS's own boot-sector-read code reads Status (0x1F7, not gated by
    // this mechanism at all) much later, with the slave still nominally
    // left selected from that same POST-time probe, and needs the real,
    // present master's actual status there, not a stuck floating one.
    // Modeling the float as a permanent property of "slave selected"
    // (rather than this two-read detection window) broke exactly that:
    // the boot loader's very first status check saw a floated BSY bit and
    // refused to even attempt reading the boot sector. See
    // PC486_REVIEW.md.
    hdd.out(0x1F6, 0xB0);  // select drive 1 (slave, never mounted)
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // release it -- arms the two-read floating window
    // Status was never part of the floating window at all -- confirm it
    // already reads as the real, present master's value even before the
    // window's two Cylinder Low/High reads are consumed.
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY -- the real master, not floating

    hdd.in(0x1F4);  // consume both floating reads, exactly as ata_detect() does
    hdd.in(0x1F5);
    // With the window spent, and the slave still nominally selected (no
    // new reset, no reselect), Cylinder Low/High now read the real
    // master's values too -- matching what the boot loader actually
    // depends on.
    EXPECT_EQ(hdd.in(0x1F4), 0x00);
    EXPECT_EQ(hdd.in(0x1F5), 0x00);
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);
}

TEST_F(Wd1003Test, IdentifyDeviceReportsTheFiveHundredFourMegabyteChsCeilingGeometry) {
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);  // IDENTIFY DEVICE
    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);  // DRQ: data ready
    std::vector<uint16_t> w;
    for (int i = 0; i < 256; ++i) w.push_back(hdd.data_in16());
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // DRQ cleared once fully drained

    // 1024 cyl x 16 head x 63 sec is not a round number picked for looks:
    // INT 13h packs the cylinder into 10 bits and the sector into 6 (1-based)
    // while the ATA task file allows 16 heads, so this is the largest
    // geometry classic CHS addressing can express at all -- the genuine
    // 504MB barrier that "maxed out" 1993-94 IDE drives ran into and that
    // EIDE LBA translation was introduced to break. See PC486_REVIEW.md.
    EXPECT_EQ(w[1], kCylinders);
    EXPECT_EQ(w[3], kHeads);
    EXPECT_EQ(w[6], kSectorsPerTrack);
    EXPECT_EQ(w[1], 1024) << "one more cylinder would not fit INT 13h's 10-bit field";
    EXPECT_EQ(w[6], 63) << "one more sector would not fit INT 13h's 6-bit field";

    // Real BIOS code (rombios.c's ata_detect()/ata_cmd_data_io()) actually
    // reads word 5 back to size its own PIO transfer loop's per-chunk word
    // count -- leaving it at zero made every real disk I/O transfer move
    // zero words per "sector" regardless of how much data our own paced READ
    // SECTORS had genuinely ready, which is what broke the FreeDOS
    // installer's auto-partition step. See PC486_REVIEW.md.
    EXPECT_EQ(w[5], 512);

    // Words 60/61: total addressable sectors, low word first. 1,032,192
    // sectors x 512 = 528,482,304 bytes = exactly 504MB.
    uint32_t total = uint32_t(w[60]) | (uint32_t(w[61]) << 16);
    EXPECT_EQ(total, uint32_t(kCapacitySectors));
    EXPECT_EQ(uint64_t(total) * 512, 528482304u);
}

TEST_F(Wd1003Test, ReadSectorsAfterPacingReturnsRealBytes) {
    MountDrive0();
    hdd.out(0x1F6, 0xA0);       // drive 0, head 0
    hdd.out(0x1F2, 1);          // 1 sector
    hdd.out(0x1F3, 3);          // sector 3 (1-based)
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);  // cylinder 0
    hdd.out(0x1F7, 0x20);       // READ SECTORS

    EXPECT_TRUE(hdd.in(0x1F7) & 0x80);  // BSY while the paced transfer is in flight
    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    // Poll via the alternate status register from here on -- unlike the
    // primary status register, reading it does NOT acknowledge (clear) the
    // pending interrupt, so it doesn't disturb the irq_pending() check below.
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ: data ready
    EXPECT_TRUE(hdd.irq_pending());

    // Sector 3 (1-based) is the 3rd 512-byte block -> stamped with index 2.
    EXPECT_EQ(hdd.data_in16() & 0xFF, 2);
}

TEST_F(Wd1003Test, ReadSectorsUsesLbaAddressingWhenDriveHeadBitSixIsSet) {
    // This machine's real firmware substitute always issues disk I/O in
    // 28-bit LBA mode (bit 6 of the Drive/Head register), converting CHS
    // to LBA in software first -- see offset_for_current_registers()'s
    // comment. A genuine bug: treating these registers as CHS regardless
    // of that bit happened to still work for LBA sector 0 (coincides with
    // CHS(0,0,1)'s offset for any geometry) but silently computed the
    // wrong offset for anything else, which is exactly what broke the
    // FreeDOS installer's auto-partition step. See PC486_REVIEW.md.
    MountDrive0();

    // LBA sector 100 -- picked so plain CHS(cyl=0,head=0,sector=100) would be
    // out of range (this geometry has only 63 sectors/track), proving the
    // registers really are being read as LBA, not CHS.
    hdd.out(0x1F6, 0xA0 | 0x40);  // drive 0, LBA mode (bit6), LBA[27:24]=0
    hdd.out(0x1F2, 1);            // 1 sector
    hdd.out(0x1F3, 100);          // LBA[7:0] = 100
    hdd.out(0x1F4, 0);            // LBA[15:8] = 0
    hdd.out(0x1F5, 0);            // LBA[23:16] = 0
    hdd.out(0x1F7, 0x20);         // READ SECTORS

    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ: data ready (not an IDNF error)
    EXPECT_EQ(hdd.data_in16() & 0xFF, 100);  // LBA sector 100 is the 100th 512-byte block
}

TEST_F(Wd1003Test, WriteSectorsAcceptsDataImmediatelyThenCommitsSynchronously) {
    // Unlike READ SECTORS, a write's completion is NOT paced via tick() --
    // it commits the instant the CPU supplies the last byte. This is a
    // deliberate compatibility concession, not an inconsistency: this
    // machine's real firmware substitute (rombios.c's ata_cmd_data_io())
    // polls for read completion via await_ide() but never waits at all
    // before checking write completion, so BSY must already be clear the
    // moment the last word lands. See pio_write_byte()'s comment and
    // PC486_REVIEW.md.
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 1);
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    hdd.out(0x1F7, 0x30);  // WRITE SECTORS

    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);  // DRQ set immediately, no seek delay for the CPU side
    for (int i = 0; i < 255; ++i) hdd.data_out16(0xABCD);
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);  // still mid-transfer: DRQ still set, not yet BSY/done
    hdd.data_out16(0xABCD);  // the 256th and last word completes the sector
    EXPECT_TRUE(hdd.in(0x3F6) & 0x40);  // DRDY, done -- synchronously, no tick() needed
    EXPECT_FALSE(hdd.in(0x3F6) & 0x80);  // and NOT still BSY
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.dirty(0));  // and the front end is told the image changed
}

TEST_F(Wd1003Test, RecalibrateAndInitializeDeviceParametersCompleteImmediately) {
    hdd.out(0x1F7, 0x10);  // RECALIBRATE
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY

    hdd.out(0x1F2, 63);           // sectors per track
    hdd.out(0x1F6, 0xA0 | 0x0F);  // heads-1 = 15 (16 heads)
    hdd.out(0x1F7, 0x91);  // INITIALIZE DEVICE PARAMETERS
    EXPECT_TRUE(hdd.irq_pending());
}

TEST_F(Wd1003Test, AbsentSlaveDriveOnlyRefusesToExecuteCommands) {
    // Drive 1 (the slave/D: position) is permanently unpopulated on this
    // machine's primary cable -- the CD-ROM lives on the secondary channel
    // instead (see atapi_cdrom.h), exactly as a real 1993-94 board cabled
    // it. Only the COMMAND REGISTER WRITE (0x1F7) actually depends on drive
    // selection on real ATA/IDE hardware -- that's what decides which
    // device's command-execution logic responds, and a genuine absent
    // Device 1 has no separate state machine to receive it, so nothing ever
    // executes. Needed so the real BIOS's ata_detect() correctly gets no
    // response when it tries to IDENTIFY a "second drive": getting this
    // wrong let a phantom device actually execute IDENTIFY and return
    // all-zero geometry, which then caused a genuine divide-by-zero inside
    // the BIOS's extended-geometry query.
    //
    // Every OTHER task-file register (Sector Count/Number/Cylinder Low/
    // High/Features), and every read of any of them, is NOT gated by
    // selection at all -- real hardware fact, not a simplification: those
    // are simple latches on a shared parallel bus that whichever device
    // is physically present absorbs regardless of the DEV bit, since
    // there's no per-device routing for them. Gating those too was a
    // second real bug, caught by a genuine HDD-only boot test: this
    // BIOS's own boot-sector-read code programs the CHS/sector-count
    // registers *before* reselecting the master (right after probing the
    // slave last during POST) -- gating those writes on the
    // then-still-selected slave silently discarded the real request,
    // corrupting the very first boot-sector read into a bogus,
    // out-of-range one. See PC486_REVIEW.md.
    MountDrive0();

    hdd.out(0x1F6, 0xB0);  // select drive 1 (bit4 set -> slave, never mounted)
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    // These writes DO land -- the only real device on this cable absorbs
    // them regardless of which drive the DEV bit claims is selected.
    EXPECT_EQ(hdd.in(0x1F2), 0x55);
    EXPECT_EQ(hdd.in(0x1F3), 0xAA);

    hdd.out(0x1F7, 0xEC);  // IDENTIFY DEVICE sent to the "absent" drive
    EXPECT_FALSE(hdd.irq_pending());  // nothing answers -> no command actually ran
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // and DRQ never got set -- no IDENTIFY data to read

    // Switching to drive 0 and issuing the exact same command now
    // actually runs it, proving the gate is specific to selection at
    // command-issue time, not some permanent side effect of the attempt above.
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);
}

TEST_F(Wd1003Test, ReadPastTheEndOfTheImageReportsIdNotFound) {
    MountDrive0(64);  // only 64 sectors of image behind a 504MB geometry
    hdd.out(0x1F6, 0xA0 | 0x40);  // LBA mode
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 200);          // LBA 200: inside the geometry, past the image
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    hdd.out(0x1F7, 0x20);
    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    EXPECT_TRUE(hdd.in(0x3F6) & 0x01);   // ERR
    EXPECT_EQ(hdd.in(0x1F1), 0x10);      // IDNF -- the closest real ATA error code
    EXPECT_FALSE(hdd.in(0x3F6) & 0x08);  // and no phantom data to read
}

TEST_F(Wd1003Test, BusyReflectsAnInFlightTransfer) {
    // Host/front-end convenience for an activity LED -- see wd1003.h.
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 3);
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    EXPECT_FALSE(hdd.busy());  // idle before the command is even issued
    hdd.out(0x1F7, 0x20);      // READ SECTORS
    EXPECT_TRUE(hdd.busy());
    for (uint64_t c = 0; c < 10'000'000 && hdd.busy(); c += 100) hdd.tick(c);
    EXPECT_FALSE(hdd.busy());  // transfer completed
}

TEST_F(Wd1003Test, MountedMediaSurvivesControllerReset) {
    MountDrive0();
    hdd.reset();
    EXPECT_TRUE(hdd.drives[0].present);
    EXPECT_EQ(hdd.drives[0].cylinders, kCylinders);
    EXPECT_EQ(hdd.drives[0].heads, kHeads);
    EXPECT_EQ(hdd.drives[0].sectors_per_track, kSectorsPerTrack);
    EXPECT_EQ(hdd.drives[0].capacity_sectors(), kCapacitySectors);
}

}  // namespace
