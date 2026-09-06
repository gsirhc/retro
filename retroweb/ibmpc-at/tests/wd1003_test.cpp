// GoogleTest suite for the WD1003-WA2-compatible hard disk controller: the
// genuine ATA-style detection sequence this machine's real BIOS substitute
// actually uses (scratch-register signature, soft reset, post-reset
// signature), IDENTIFY DEVICE geometry, and paced READ/WRITE SECTORS via
// the real atomic 16-bit data register path.

#include <gtest/gtest.h>

#include "wd1003.h"

#include <vector>

namespace {

using ibmpcat::Wd1003;

std::vector<uint8_t> MakeImage(long cyl, int heads, int spt) {
    std::vector<uint8_t> img(std::size_t(cyl) * heads * spt * 512, 0);
    for (std::size_t i = 0; i < img.size(); i += 512) img[i] = uint8_t((i / 512) & 0xFF);
    return img;
}

class Wd1003Test : public ::testing::Test {
protected:
    Wd1003 hdd;
    void SetUp() override { hdd.reset(); }
};

TEST_F(Wd1003Test, ScratchRegisterSignatureDetection) {
    // The real BIOS detection sequence (confirmed against rombios.c's
    // ata_detect(), see IBM_PCAT_REVIEW.md): write a scratch pattern to
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
    // status non-zero.
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
    // never come -- caught by an actual HDD-only boot test. See
    // IBM_PCAT_REVIEW.md.
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
    // refused to even attempt reading the boot sector. Caught by an
    // actual HDD-only boot test. See IBM_PCAT_REVIEW.md.
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

TEST_F(Wd1003Test, IdentifyDeviceReportsRealGeometry) {
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);  // IDENTIFY DEVICE
    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);  // DRQ: data ready
    hdd.data_in16();                   // word 0: general configuration, not checked here
    uint16_t word1 = hdd.data_in16();  // cylinders
    hdd.data_in16();                   // word 2: unused
    uint16_t word3 = hdd.data_in16();  // heads
    hdd.data_in16();                   // word 4: unused
    uint16_t word5 = hdd.data_in16();  // bytes per sector
    EXPECT_EQ(word1, 733);
    EXPECT_EQ(word3, 5);
    // Real BIOS code (rombios.c's ata_detect()/ata_cmd_data_io()) actually
    // reads this field back to size its own PIO transfer loop's per-chunk
    // word count -- leaving it at zero made every real disk I/O transfer
    // zero words per "sector" regardless of how much data our own paced
    // READ SECTORS had genuinely ready, which is what broke the FreeDOS
    // installer's auto-partition step. See IBM_PCAT_REVIEW.md.
    EXPECT_EQ(word5, 512);
    // Drain the rest of the 512-byte block.
    for (int i = 0; i < (256 - 6); ++i) hdd.data_in16();
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // DRQ cleared once fully drained
}

TEST_F(Wd1003Test, ReadSectorsAfterPacingReturnsRealBytes) {
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
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
    // FreeDOS installer's auto-partition step. See IBM_PCAT_REVIEW.md.
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());

    // LBA sector 40 -- picked so plain CHS(cyl=0,head=0,sector=40) would be
    // out of range (this geometry has only 17 sectors/track), proving the
    // registers really are being read as LBA, not CHS.
    hdd.out(0x1F6, 0xA0 | 0x40);  // drive 0, LBA mode (bit6), LBA[27:24]=0
    hdd.out(0x1F2, 1);            // 1 sector
    hdd.out(0x1F3, 40);           // LBA[7:0] = 40
    hdd.out(0x1F4, 0);            // LBA[15:8] = 0
    hdd.out(0x1F5, 0);            // LBA[23:16] = 0
    hdd.out(0x1F7, 0x20);         // READ SECTORS

    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ: data ready (not an IDNF error)
    EXPECT_EQ(hdd.data_in16() & 0xFF, 40);  // LBA sector 40 is the 40th 512-byte block
}

TEST_F(Wd1003Test, WriteSectorsAcceptsDataImmediatelyThenCommitsSynchronously) {
    // Unlike READ SECTORS, a write's completion is NOT paced via tick() --
    // it commits the instant the CPU supplies the last byte. This is a
    // deliberate compatibility concession, not an inconsistency: this
    // machine's real firmware substitute (rombios.c's ata_cmd_data_io())
    // polls for read completion via await_ide() but never waits at all
    // before checking write completion, so BSY must already be clear the
    // moment the last word lands. See pio_write_byte()'s comment and
    // IBM_PCAT_REVIEW.md.
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
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
}

TEST_F(Wd1003Test, RecalibrateAndInitializeDeviceParametersCompleteImmediately) {
    hdd.out(0x1F7, 0x10);  // RECALIBRATE
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY

    hdd.out(0x1F2, 17);   // sectors per track
    hdd.out(0x1F6, 0xA0 | 0x04);  // heads-1 = 4 (5 heads)
    hdd.out(0x1F7, 0x91);  // INITIALIZE DEVICE PARAMETERS
    EXPECT_TRUE(hdd.irq_pending());
}

TEST_F(Wd1003Test, AbsentSlaveDriveOnlyRefusesToExecuteCommands) {
    // Drive 1 (the slave/D: position) is permanently unpopulated on this
    // machine's real single-device cable. Only the COMMAND REGISTER WRITE
    // (0x1F7) actually depends on drive selection on real ATA/IDE
    // hardware -- that's what decides which device's command-execution
    // logic responds, and a genuine absent Device 1 has no separate state
    // machine to receive it, so nothing ever executes. Needed so the real
    // BIOS's ata_detect() correctly gets no response when it tries to
    // IDENTIFY a "second drive": getting this wrong let a phantom device
    // actually execute IDENTIFY and return all-zero geometry, which then
    // caused a genuine divide-by-zero inside the BIOS's extended-geometry
    // query.
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
    // out-of-range one. See IBM_PCAT_REVIEW.md.
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());

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

TEST_F(Wd1003Test, MountedMediaSurvivesControllerReset) {
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
    hdd.reset();
    EXPECT_TRUE(hdd.drives[0].present);
}

}  // namespace
