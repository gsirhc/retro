#include <gtest/gtest.h>

#include "wd1003.h"

#include <cmath>
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

class Wd1003CommandTest : public Wd1003Test {
protected:
    std::vector<uint8_t> img = MakeImage(733, 5, 17);
    uint64_t now = 0;
    void SetUp() override {
        Wd1003Test::SetUp();
        hdd.mount(0, img.data(), img.size());
    }
    void Chs(int cyl, int head, int sector, int count) {
        hdd.out(0x1F6, uint8_t(0xA0 | head));
        hdd.out(0x1F2, uint8_t(count));
        hdd.out(0x1F3, uint8_t(sector));
        hdd.out(0x1F4, uint8_t(cyl & 0xFF));
        hdd.out(0x1F5, uint8_t(cyl >> 8));
    }
    static constexpr double kRev = 8e6 / 60.0;
    static constexpr double kSlot = kRev / 17.0;
    void RunTo(uint64_t c) { while (now < c) hdd.tick(now += 100); }
    uint64_t RunWhileBusy() {
        uint64_t start = now;
        while ((hdd.in(0x3F6) & 0x80) && now - start < 100'000'000) hdd.tick(now += 100);
        return now - start;
    }
    void Format(int cyl, int head, const uint8_t (&table)[34]) {
        Chs(cyl, head, 1, 17);
        hdd.out(0x1F7, 0x50);
        for (int i = 0; i < 256; ++i) {
            uint16_t w = i < 17 ? uint16_t(table[i * 2] | (table[i * 2 + 1] << 8)) : 0;
            hdd.data_out16(w);
        }
        RunWhileBusy();
        hdd.in(0x1F7);
    }
};

TEST_F(Wd1003CommandTest, SeekCompletesWithAnInterrupt) {
    Chs(400, 0, 1, 1);
    hdd.out(0x1F7, 0x7F);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_EQ(hdd.in(0x1F7) & 0x51, 0x50);
}

TEST_F(Wd1003CommandTest, WritePrecompDoesNotLandInTheErrorRegister) {
    hdd.out(0x1F1, 0x20);
    EXPECT_EQ(hdd.in(0x1F1), 0x00);
}

TEST_F(Wd1003CommandTest, DiagnoseReportsNoErrorsAndClearsTheTaskFile) {
    Chs(300, 3, 9, 5);
    hdd.out(0x1F7, 0x90);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_EQ(hdd.in(0x1F1), 0x01);
    EXPECT_FALSE(hdd.in(0x1F7) & 0x01);
    EXPECT_EQ(hdd.in(0x1F2), 1);
    EXPECT_EQ(hdd.in(0x1F4), 0);
    EXPECT_EQ(hdd.in(0x1F5), 0);
    EXPECT_EQ(hdd.in(0x1F6), 0);
}

TEST_F(Wd1003CommandTest, ReadVerifyPastTheEndReportsIdnf) {
    Chs(732, 4, 17, 2);
    hdd.out(0x1F7, 0x41);
    RunWhileBusy();
    EXPECT_TRUE(hdd.in(0x1F7) & 0x01);
    EXPECT_EQ(hdd.in(0x1F1), 0x10);
}

TEST_F(Wd1003CommandTest, MultiSectorReadInterruptsPerSectorAndNotAtTheEnd) {
    Chs(0, 0, 1, 3);
    hdd.out(0x1F7, 0x20);
    for (int s = 0; s < 3; ++s) {
        RunWhileBusy();
        ASSERT_TRUE(hdd.irq_pending()) << "sector " << s;
        EXPECT_EQ(hdd.in(0x1F7) & 0x88, 0x08);
        EXPECT_FALSE(hdd.irq_pending());
        EXPECT_EQ(hdd.data_in16() & 0xFF, s);
        for (int i = 1; i < 256; ++i) hdd.data_in16();
    }
    EXPECT_FALSE(hdd.irq_pending());
    EXPECT_EQ(hdd.in(0x1F7) & 0xC8, 0x40);
}

TEST_F(Wd1003CommandTest, BadBlockMarkFailsReadVerifyAndWrite) {
    uint8_t table[34] = {};
    for (int i = 0; i < 17; ++i) table[i * 2 + 1] = uint8_t(i + 1);
    table[4 * 2] = 0x80;  // sector 5
    Format(10, 2, table);

    Chs(10, 2, 3, 4);
    hdd.out(0x1F7, 0x20);
    for (int s = 0; s < 2; ++s) {
        RunWhileBusy();
        ASSERT_TRUE(hdd.in(0x1F7) & 0x08);
        for (int i = 0; i < 256; ++i) hdd.data_in16();
    }
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F7) & 0x09, 0x01);
    EXPECT_EQ(hdd.in(0x1F1), 0x80);

    Chs(10, 2, 1, 17);
    hdd.out(0x1F7, 0x40);
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F1), 0x80);

    Chs(10, 2, 4, 2);
    hdd.out(0x1F7, 0x30);
    for (int i = 0; i < 512; ++i) hdd.data_out16(0x7777);
    EXPECT_EQ(hdd.in(0x1F1), 0x80);
    const auto& d = hdd.drives[0];
    EXPECT_EQ(d.image[std::size_t(d.offset_for(10, 2, 4))], 0x77);
    EXPECT_EQ(d.image[std::size_t(d.offset_for(10, 2, 5))], 0x00);
}

TEST_F(Wd1003CommandTest, ReformattingClearsABadBlockAndRemountForgetsThem) {
    uint8_t table[34] = {};
    for (int i = 0; i < 17; ++i) table[i * 2 + 1] = uint8_t(i + 1);
    table[0] = 0x80;
    Format(0, 0, table);
    EXPECT_EQ(hdd.drives[0].bad_sectors.size(), 1u);
    table[0] = 0x00;
    Format(0, 0, table);
    EXPECT_TRUE(hdd.drives[0].bad_sectors.empty());

    table[0] = 0x80;
    Format(0, 0, table);
    hdd.reset();
    EXPECT_EQ(hdd.drives[0].bad_sectors.size(), 1u);
    hdd.mount(0, img.data(), img.size());
    EXPECT_TRUE(hdd.drives[0].bad_sectors.empty());
}

TEST_F(Wd1003CommandTest, InterruptHeldWhileIenIsOffFiresWhenEnabled) {
    hdd.out(0x3F6, 0x02);
    hdd.out(0x1F7, 0x10);
    EXPECT_FALSE(hdd.irq_pending());
    hdd.out(0x3F6, 0x00);
    EXPECT_TRUE(hdd.irq_pending());
}

TEST_F(Wd1003CommandTest, ReadVerifyFollowsTheFactoryInterleaveAndInterruptsOnce) {
    // sectors 1-4 at 3:1 sit in slots 0, 3, 6 and 9
    Chs(0, 0, 1, 4);
    hdd.out(0x1F7, 0x40);
    EXPECT_FALSE(hdd.irq_pending());
    EXPECT_NEAR(double(RunWhileBusy()), 10 * kSlot, 200.0);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_EQ(hdd.in(0x1F7) & 0x09, 0x00);
}

TEST_F(Wd1003CommandTest, ReadWaitsForTheSectorToComeRound) {
    RunTo(uint64_t(5 * kSlot));
    Chs(0, 0, 1, 1);
    hdd.out(0x1F7, 0x20);
    EXPECT_NEAR(double(RunWhileBusy()), kRev - 4 * kSlot, 200.0);
}

TEST_F(Wd1003CommandTest, NextSectorIsBusyUntilItsSlotPasses) {
    Chs(0, 0, 1, 2);
    hdd.out(0x1F7, 0x20);
    RunWhileBusy();
    for (int i = 0; i < 256; ++i) hdd.data_in16();
    EXPECT_TRUE(hdd.in(0x3F6) & 0x80);
    RunWhileBusy();
    EXPECT_NEAR(double(now), 4 * kSlot, 200.0);
}

TEST_F(Wd1003CommandTest, FormatTrackStartsAtIndexAndZeroesTheTrack) {
    img.assign(img.size(), 0xE5);
    hdd.mount(0, img.data(), img.size());
    RunTo(50'000);
    Chs(2, 1, 1, 17);
    hdd.out(0x1F7, 0x50);
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);
    for (int i = 0; i < 256; ++i) hdd.data_out16(i < 17 ? uint16_t((i + 1) << 8) : 0);
    RunWhileBusy();
    EXPECT_NEAR(double(now), 2 * kRev, 200.0);
    EXPECT_TRUE(hdd.irq_pending());
    const auto& d = hdd.drives[0];
    for (int r = 1; r <= 17; ++r) EXPECT_EQ(d.image[std::size_t(d.offset_for(2, 1, r)) + 100], 0) << r;
    EXPECT_EQ(d.image[std::size_t(d.offset_for(2, 2, 1))], 0xE5);
    EXPECT_EQ(d.image[std::size_t(d.offset_for(2, 0, 17))], 0xE5);
    EXPECT_TRUE(hdd.dirty(0));
}

TEST_F(Wd1003CommandTest, OneToOneInterleaveMissesTheNextSectorWhileTheHostDrains) {
    uint8_t table[34] = {};
    for (int i = 0; i < 16; ++i) table[i * 2 + 1] = uint8_t(i + 1);
    Format(0, 0, table);
    uint64_t start = uint64_t(std::ceil(double(now) / kRev) * kRev);
    RunTo(start - 150);
    Chs(0, 0, 1, 2);
    hdd.out(0x1F7, 0x20);
    RunWhileBusy();
    for (int i = 0; i < 256; ++i) hdd.data_in16();
    RunWhileBusy();
    EXPECT_NEAR(double(now - start), kRev + 2 * kSlot, 200.0);
    for (int i = 0; i < 256; ++i) hdd.data_in16();

    Chs(0, 0, 17, 1);
    uint64_t t0 = now;
    hdd.out(0x1F7, 0x21);
    RunWhileBusy();
    EXPECT_NEAR(double(now - t0), 2 * kRev, 200.0);
    EXPECT_EQ(hdd.in(0x1F1), 0x10);
}

TEST_F(Wd1003CommandTest, MissingIdIsSearchedForTenRevolutions) {
    Chs(733, 0, 1, 1);
    hdd.out(0x1F7, 0x20);
    EXPECT_NEAR(double(RunWhileBusy()), 10 * kRev, 200.0);
    EXPECT_EQ(hdd.in(0x1F7) & 0x01, 0x01);
    EXPECT_EQ(hdd.in(0x1F1), 0x10);
}

TEST_F(Wd1003CommandTest, EccMatchesTheWd11c00Generator) {
    std::vector<uint8_t> zeros(512, 0);
    EXPECT_EQ(Wd1003::data_ecc(zeros.data()), 0x15CFE3A9u);
}

TEST_F(Wd1003CommandTest, ReadLongSendsTheDataThenItsFourEccBytes) {
    Chs(0, 0, 3, 1);
    hdd.out(0x1F7, 0x22);
    RunWhileBusy();
    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);
    EXPECT_EQ(hdd.data_in16(), 2);
    for (int i = 1; i < 256; ++i) hdd.data_in16();
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);
    uint8_t ecc[4];
    for (auto& b : ecc) b = hdd.in(0x1F0);
    EXPECT_EQ(ecc[0], 0x8A);
    EXPECT_EQ(ecc[1], 0xC2);
    EXPECT_EQ(ecc[2], 0xDF);
    EXPECT_EQ(ecc[3], 0x03);
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);
}

class Wd1003EccTest : public Wd1003CommandTest {
protected:
    // WRITE LONG sector 1 of cylinder 5 head 0 with this data and ECC.
    void WriteLong(const std::vector<uint8_t>& data, uint32_t ecc) {
        Chs(5, 0, 1, 1);
        hdd.out(0x1F7, 0x32);
        for (std::size_t i = 0; i < 512; i += 2) hdd.data_out16(uint16_t(data[i] | (data[i + 1] << 8)));
        for (int i = 3; i >= 0; --i) hdd.out(0x1F0, uint8_t(ecc >> (8 * i)));
        hdd.in(0x1F7);
    }
    std::vector<uint8_t> Pattern() {
        std::vector<uint8_t> d(512);
        for (std::size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 7 + 3);
        return d;
    }
    std::vector<uint8_t> ReadBack(uint8_t* status, uint64_t* took) {
        Chs(5, 0, 1, 1);
        uint64_t t0 = now;
        hdd.out(0x1F7, 0x20);
        RunWhileBusy();
        *took = now - t0;
        *status = hdd.in(0x1F7);
        std::vector<uint8_t> out;
        for (int i = 0; i < 256; ++i) {
            uint16_t w = hdd.data_in16();
            out.push_back(uint8_t(w));
            out.push_back(uint8_t(w >> 8));
        }
        return out;
    }
};

TEST_F(Wd1003EccTest, WriteLongWithTheRightEccReadsClean) {
    auto data = Pattern();
    WriteLong(data, Wd1003::data_ecc(data.data()));
    EXPECT_TRUE(hdd.drives[0].ecc_override.empty());
    uint8_t st;
    uint64_t took;
    EXPECT_EQ(ReadBack(&st, &took), data);
    EXPECT_EQ(st & 0x05, 0x00);
    EXPECT_LT(double(took), kRev + 200);
}

TEST_F(Wd1003EccTest, AFiveBitBurstIsCorrectedAfterOneReread) {
    auto good = Pattern();
    uint32_t ecc = Wd1003::data_ecc(good.data());
    auto bad = good;
    bad[100] ^= 0x0E;
    bad[101] ^= 0x80;
    WriteLong(bad, ecc);
    uint8_t st;
    uint64_t took;
    EXPECT_EQ(ReadBack(&st, &took), good);
    EXPECT_EQ(st & 0x0D, 0x0C);
    EXPECT_GT(double(took), kRev);
    EXPECT_LT(double(took), 2 * kRev + 200);
    EXPECT_EQ(hdd.drives[0].image[std::size_t(hdd.drives[0].offset_for(5, 0, 1)) + 100], bad[100]);
}

TEST_F(Wd1003EccTest, AWiderErrorIsUncorrectableButTheDataStillComes) {
    auto good = Pattern();
    uint32_t ecc = Wd1003::data_ecc(good.data());
    auto bad = good;
    bad[10] ^= 0x01;
    bad[300] ^= 0x80;
    WriteLong(bad, ecc);
    uint8_t st;
    uint64_t took;
    EXPECT_EQ(ReadBack(&st, &took), bad);
    EXPECT_EQ(st & 0x09, 0x09);
    EXPECT_EQ(hdd.in(0x1F1), 0x40);
    EXPECT_GT(double(took), 8 * kRev);
    EXPECT_LT(double(took), 9 * kRev + 200);
    EXPECT_EQ(hdd.in(0x1F7) & 0x89, 0x01);
}

TEST_F(Wd1003EccTest, AMultiSectorReadStopsAfterTheUncorrectableSector) {
    auto bad = Pattern();
    WriteLong(bad, ~Wd1003::data_ecc(bad.data()));
    Chs(5, 0, 1, 3);
    hdd.out(0x1F7, 0x20);
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F1), 0x40);
    for (int i = 0; i < 256; ++i) hdd.data_in16();
    EXPECT_EQ(hdd.in(0x1F7) & 0x89, 0x01);
    EXPECT_FALSE(hdd.busy());
}

TEST_F(Wd1003EccTest, ReadVerifyCorrectsOrReportsTheSameWay) {
    auto data = Pattern();
    uint32_t ecc = Wd1003::data_ecc(data.data());
    WriteLong(data, ecc ^ 0x00000300);
    Chs(5, 0, 1, 1);
    hdd.out(0x1F7, 0x40);
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F7) & 0x05, 0x04);
    WriteLong(data, ~ecc);
    Chs(5, 0, 1, 1);
    hdd.out(0x1F7, 0x40);
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F7) & 0x01, 0x01);
    EXPECT_EQ(hdd.in(0x1F1), 0x40);
}

TEST_F(Wd1003EccTest, ReadLongReturnsTheWrittenEccAndANormalWriteReplacesIt) {
    auto data = Pattern();
    WriteLong(data, 0x12345678);
    Chs(5, 0, 1, 1);
    hdd.out(0x1F7, 0x22);
    RunWhileBusy();
    EXPECT_EQ(hdd.in(0x1F7) & 0x01, 0x00);
    for (int i = 0; i < 256; ++i) hdd.data_in16();
    EXPECT_EQ(hdd.in(0x1F0), 0x12);
    EXPECT_EQ(hdd.in(0x1F0), 0x34);
    EXPECT_EQ(hdd.in(0x1F0), 0x56);
    EXPECT_EQ(hdd.in(0x1F0), 0x78);

    Chs(5, 0, 1, 1);
    hdd.out(0x1F7, 0x30);
    for (int i = 0; i < 256; ++i) hdd.data_out16(0);
    EXPECT_TRUE(hdd.drives[0].ecc_override.empty());
    hdd.mount(0, img.data(), img.size());
    WriteLong(data, 0x12345678);
    hdd.mount(0, img.data(), img.size());
    EXPECT_TRUE(hdd.drives[0].ecc_override.empty());
}

TEST_F(Wd1003CommandTest, UnknownCommandsAbort) {
    for (uint8_t cmd : {0xE0, 0x00, 0xA0}) {
        Chs(0, 0, 1, 1);
        hdd.out(0x1F7, cmd);
        EXPECT_EQ(hdd.in(0x1F7) & 0x01, 0x01) << int(cmd);
        EXPECT_EQ(hdd.in(0x1F1), 0x04) << int(cmd);
    }
}

}  // namespace
