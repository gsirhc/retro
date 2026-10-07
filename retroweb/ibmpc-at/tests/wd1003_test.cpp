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
    // BIOS detection (rombios.c ata_detect()): write a scratch pattern and read it back.
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
    // Soft reset (SRST) restores the post-reset signature: count/number = 1, cylinder = 0, status non-zero.
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // reset happens on this edge
    EXPECT_EQ(hdd.in(0x1F2), 0x01);
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);  // cylinder low
    EXPECT_EQ(hdd.in(0x1F5), 0x00);  // cylinder high
    EXPECT_NE(hdd.in(0x3F6) /* alt status, doesn't clear IRQ */, 0x00);
}

TEST_F(Wd1003Test, SoftResetSignalsAbsenceForTheSlaveViaFloatedCylinderRegisters) {
    // ata_detect() tells a real device from none by Cylinder Low/High after reset:
    // 0x00/0x00 present, 0xFF/0xFF floated. Getting it wrong made the absent slave
    // look real and the BIOS panicked after a ~32s timeout (IBM_PCAT_REVIEW.md).
    hdd.out(0x1F6, 0xB0);  // select drive 1 (slave, never mounted)
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // release it
    EXPECT_EQ(hdd.in(0x1F2), 0x01);  // sector count/number still read back 1/1...
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0xFF);  // ...but cylinder low/high floats, signaling absence
    EXPECT_EQ(hdd.in(0x1F5), 0xFF);

    // The master keeps its real 0x00/0x00 signature after the slave-selected reset.
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x3F6, 0x04);
    hdd.out(0x3F6, 0x00);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);
    EXPECT_EQ(hdd.in(0x1F5), 0x00);
}

TEST_F(Wd1003Test, FloatedSignatureIsATwoReadWindowNotAPersistentState) {
    // The float lasts only the two Cylinder Low/High reads ata_detect() makes. The
    // boot loader later reads Status with the slave still selected and needs the
    // master's value (IBM_PCAT_REVIEW.md).
    hdd.out(0x1F6, 0xB0);  // select drive 1 (slave, never mounted)
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);  // release it -- arms the two-read floating window
    // Status was never floated.
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY -- the real master, not floating

    hdd.in(0x1F4);  // consume both floating reads, exactly as ata_detect() does
    hdd.in(0x1F5);
    // With the window spent, Cylinder Low/High read the master's values.
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
    // The BIOS reads this field to size its PIO loop. Zero broke the FreeDOS
    // installer's auto-partition step (IBM_PCAT_REVIEW.md).
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
    // Poll alternate status: reading it does not clear the pending interrupt.
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ: data ready
    EXPECT_TRUE(hdd.irq_pending());

    // Sector 3 (1-based) is the 3rd 512-byte block -> stamped with index 2.
    EXPECT_EQ(hdd.data_in16() & 0xFF, 2);
}

TEST_F(Wd1003Test, ReadSectorsUsesLbaAddressingWhenDriveHeadBitSixIsSet) {
    // The BIOS issues all I/O in 28-bit LBA mode (Drive/Head bit 6). Treating the
    // registers as CHS only worked for LBA 0 (IBM_PCAT_REVIEW.md).
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());

    // LBA 40 would be out of range as CHS (17 sectors/track), proving LBA is used.
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
    // Unlike READ SECTORS, a write commits when the last byte arrives, with no
    // tick() pacing. rombios.c ata_cmd_data_io() never awaits BSY after a write.
    // See pio_write_byte().
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
    // Drive 1 is permanently absent. Only command-register writes (0x1F7) are gated
    // on selection, so IDENTIFY to the "absent" drive does nothing (a phantom
    // all-zero geometry caused a divide-by-zero in the BIOS). Other task-file
    // writes and all reads are ungated, as the BIOS programs CHS before reselecting
    // the master. See IBM_PCAT_REVIEW.md.
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());

    hdd.out(0x1F6, 0xB0);  // select drive 1 (bit4 set -> slave, never mounted)
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    // These writes land: the one real device absorbs them whatever DEV says.
    EXPECT_EQ(hdd.in(0x1F2), 0x55);
    EXPECT_EQ(hdd.in(0x1F3), 0xAA);

    hdd.out(0x1F7, 0xEC);  // IDENTIFY DEVICE sent to the "absent" drive
    EXPECT_FALSE(hdd.irq_pending());  // nothing answers -> no command actually ran
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // and DRQ never got set -- no IDENTIFY data to read

    // Drive 0 runs the same command, so the gate is per command issue.
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);
}

TEST_F(Wd1003Test, BusyReflectsAnInFlightTransfer) {
    // Activity LED helper (wd1003.h).
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
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
    auto img = MakeImage(733, 5, 17);
    hdd.mount(0, img.data(), img.size());
    hdd.reset();
    EXPECT_TRUE(hdd.drives[0].present);
}

}  // namespace
