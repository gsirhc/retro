#include <gtest/gtest.h>

#include "fdc765.h"

#include <cstring>
#include <vector>

namespace {

using ibmpcat::Fdc765;

std::vector<uint8_t> MakeImage(std::size_t cyl, int heads, int spt) {
    std::vector<uint8_t> img(cyl * heads * spt * 512, 0);
    // Stamp each sector's first byte with its index.
    for (std::size_t i = 0; i < img.size(); i += 512) img[i] = uint8_t((i / 512) & 0xFF);
    return img;
}

class Fdc765Test : public ::testing::Test {
protected:
    Fdc765 fdc;
    void SetUp() override { fdc.reset(); }

    void PowerOnMotorAndSelect(int drive) {
        // Motor on, ~RESET high, DMA/IRQ enable, drive select. Leaving reset raises
        // an interrupt, so acknowledge it with SENSE INTERRUPT STATUS first.
        uint8_t bit = drive == 0 ? 0x10 : 0x20;
        fdc.out(0x3F2, uint8_t(0x04 | 0x08 | bit | drive));
        fdc.clear_irq();
    }
};

TEST_F(Fdc765Test, DoesNotOwnPortThreeF6) {
    // 0x3F6 belongs to the HDD (wd1003.h). A too-wide FDC range would steal it.
    EXPECT_FALSE(fdc.owns(0x3F6));
    EXPECT_TRUE(fdc.owns(0x3F5));
    EXPECT_TRUE(fdc.owns(0x3F7));
}

TEST_F(Fdc765Test, SpecifyIsAcceptedWithoutResult) {
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x03);  // SPECIFY
    fdc.out(0x3F5, 0xDF);  // SRT/HUT
    fdc.out(0x3F5, 0x02);  // HLT/ND
    EXPECT_EQ(fdc.in(0x3F4) & 0x10, 0x00);  // FDC busy cleared -- command completed immediately
    EXPECT_EQ(fdc.in(0x3F4) & 0x80, 0x80);  // RQM: ready for the next command
}

TEST_F(Fdc765Test, RecalibrateCompletesAfterPacedDelayAndReportsViaSenseInterrupt) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x07);  // RECALIBRATE
    fdc.out(0x3F5, 0x00);  // drive 0

    EXPECT_FALSE(fdc.irq_pending());
    for (int i = 0; i < 1000 && !fdc.irq_pending(); ++i) fdc.tick(uint64_t(i) * 1000);
    EXPECT_TRUE(fdc.irq_pending());

    fdc.out(0x3F5, 0x08);  // SENSE INTERRUPT STATUS
    EXPECT_EQ(fdc.in(0x3F4) & 0xC0, 0xC0);  // RQM+DIO both set: result phase, a byte is ready to read
    uint8_t st0 = fdc.in(0x3F5);
    uint8_t pcn = fdc.in(0x3F5);
    EXPECT_EQ(st0 & 0x20, 0x20);  // seek-end
    EXPECT_EQ(pcn, 0);            // recalibrated to cylinder 0
}

TEST_F(Fdc765Test, SenseInterruptWithNothingPendingReportsInvalidCommand) {
    fdc.out(0x3F5, 0x08);
    EXPECT_EQ(fdc.in(0x3F5), 0x80);
}

TEST_F(Fdc765Test, ReadDataTransfersTheRequestedSectorAfterPacing) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);

    // READ DATA (0xE6 = MT|MFM|SK|base 0x06), sector 3, 1 sector requested.
    fdc.out(0x3F5, 0xE6);
    fdc.out(0x3F5, 0x00);  // drive 0, head 0
    fdc.out(0x3F5, 0x00);  // C
    fdc.out(0x3F5, 0x00);  // H
    fdc.out(0x3F5, 0x03);  // R (sector 3)
    fdc.out(0x3F5, 0x02);  // N (512 bytes)
    fdc.out(0x3F5, 0x03);  // EOT (last sector = 3, i.e. just this one)
    fdc.out(0x3F5, 0x1B);  // GPL
    fdc.out(0x3F5, 0xFF);  // DTL

    EXPECT_FALSE(fdc.transfer_ready());
    for (int i = 0; i < 100000 && !fdc.transfer_ready(); ++i) fdc.tick(uint64_t(i) * 100);
    ASSERT_TRUE(fdc.transfer_ready());
    EXPECT_FALSE(fdc.transfer_is_write());
    ASSERT_EQ(fdc.transfer_length(), std::size_t(512));

    uint8_t *ptr = fdc.transfer_image_ptr();
    ASSERT_NE(ptr, nullptr);
    // Sector 3 (1-based) is the 3rd 512-byte block -> stamped with index 2.
    EXPECT_EQ(ptr[0], 2);

    fdc.finish_transfer(512);
    EXPECT_EQ(fdc.in(0x3F4) & 0xC0, 0xC0);  // result phase, byte ready
    uint8_t st0 = fdc.in(0x3F5);
    EXPECT_EQ(st0, 0x00);  // normal termination
}

TEST_F(Fdc765Test, WriteDataMarksDriveDirty) {
    auto img = MakeImage(40, 2, 9);
    fdc.mount(1, img.data(), img.size());  // drive B: (360KB)
    PowerOnMotorAndSelect(1);
    ASSERT_FALSE(fdc.dirty(1));

    fdc.out(0x3F5, 0xC5);  // WRITE DATA (MT|MFM|base 0x05)
    fdc.out(0x3F5, 0x01);  // drive 1, head 0
    fdc.out(0x3F5, 0x00);
    fdc.out(0x3F5, 0x00);
    fdc.out(0x3F5, 0x01);
    fdc.out(0x3F5, 0x02);
    fdc.out(0x3F5, 0x01);
    fdc.out(0x3F5, 0x1B);
    fdc.out(0x3F5, 0xFF);

    for (int i = 0; i < 100000 && !fdc.transfer_ready(); ++i) fdc.tick(uint64_t(i) * 100);
    ASSERT_TRUE(fdc.transfer_ready());
    EXPECT_TRUE(fdc.transfer_is_write());
    uint8_t *ptr = fdc.transfer_image_ptr();
    ASSERT_NE(ptr, nullptr);
    ptr[0] = 0xAB;  // simulate chipset having copied a byte in from RAM
    fdc.finish_transfer(512);
    EXPECT_TRUE(fdc.dirty(1));
}

TEST_F(Fdc765Test, InterruptClearsOnFirstResultByteNotTheWholePhase) {
    // IRQ6 drops on the first result byte (ST0), not after the full drain (IBM_PCAT_REVIEW.md §8).
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);

    fdc.out(0x3F5, 0xE6);  // READ DATA, one sector -> 7-byte result phase
    fdc.out(0x3F5, 0x00); fdc.out(0x3F5, 0x00); fdc.out(0x3F5, 0x00);
    fdc.out(0x3F5, 0x01); fdc.out(0x3F5, 0x02); fdc.out(0x3F5, 0x01);
    fdc.out(0x3F5, 0x1B); fdc.out(0x3F5, 0xFF);
    for (int i = 0; i < 100000 && !fdc.transfer_ready(); ++i) fdc.tick(uint64_t(i) * 100);
    ASSERT_TRUE(fdc.transfer_ready());
    fdc.finish_transfer(512);
    ASSERT_TRUE(fdc.irq_pending());

    fdc.in(0x3F5);  // read only ST0 -- 6 more result bytes are still unread
    EXPECT_FALSE(fdc.irq_pending());
    EXPECT_TRUE(fdc.in(0x3F4) & 0x10);  // controller still busy -- result phase isn't fully drained yet
}

TEST_F(Fdc765Test, DiskChangeLineSetByMountAndClearedBySeek) {
    // DSKCHG asserts on mount and clears on the next step. Swap-aware software polls it.
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80);  // just mounted -- changed

    fdc.out(0x3F5, 0x07); fdc.out(0x3F5, 0x00);  // RECALIBRATE drive 0
    EXPECT_FALSE(fdc.in(0x3F7) & 0x80);  // stepped -- change acknowledged

    // Swapping media again re-asserts it, independent of the prior seek.
    auto img2 = MakeImage(80, 2, 15);
    fdc.mount(0, img2.data(), img2.size());
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80);
}

TEST_F(Fdc765Test, MountedMediaSurvivesControllerReset) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    fdc.reset();
    EXPECT_TRUE(fdc.mounted(0));
}

TEST_F(Fdc765Test, ResetDropsIntUntilItIsReleased) {
    fdc.out(0x3F2, 0x1C);
    ASSERT_TRUE(fdc.irq_pending());
    fdc.out(0x3F2, 0x18);
    EXPECT_FALSE(fdc.irq_pending());
    fdc.out(0x3F2, 0x1C);
    EXPECT_TRUE(fdc.irq_pending());
}

TEST_F(Fdc765Test, FormatTrackFillsTheNamedSectorsAfterOneRevolution) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x0F); fdc.out(0x3F5, 0x04); fdc.out(0x3F5, 0x05);  // SEEK head 1, cylinder 5
    uint64_t c = 0;
    for (; c < 1'000'000 && !fdc.irq_pending(); c += 100) fdc.tick(c);
    fdc.out(0x3F5, 0x08); fdc.in(0x3F5); fdc.in(0x3F5);

    const uint8_t cmd[] = {0x4D, 0x04, 0x02, 0x0F, 0x54, 0xF6};
    for (uint8_t b : cmd) fdc.out(0x3F5, b);
    ASSERT_EQ(fdc.transfer_length(), std::size_t(15 * 4));
    uint64_t start = c;
    for (; c < start + 10'000'000 && !fdc.transfer_ready(); c += 100) fdc.tick(c);
    ASSERT_TRUE(fdc.transfer_ready());
    EXPECT_NEAR(double(c - start), 8e6 / 6.0, 200.0);
    EXPECT_TRUE(fdc.transfer_is_write());
    uint8_t* ids = fdc.transfer_image_ptr();
    for (int i = 0; i < 15; ++i) {
        ids[i * 4] = 5; ids[i * 4 + 1] = 1; ids[i * 4 + 2] = uint8_t(i + 1); ids[i * 4 + 3] = 2;
    }
    fdc.finish_transfer(15 * 4);
    EXPECT_TRUE(fdc.irq_pending());
    EXPECT_EQ(fdc.in(0x3F5), 0x00);
    const auto& d = fdc.drives[0];
    for (int r = 1; r <= 15; ++r) {
        EXPECT_EQ(d.image[std::size_t(d.offset_for(5, 1, r))], 0xF6) << r;
        EXPECT_EQ(d.image[std::size_t(d.offset_for(5, 1, r)) + 511], 0xF6) << r;
    }
    EXPECT_EQ(d.image[std::size_t(d.offset_for(5, 0, 1))], uint8_t(d.offset_for(5, 0, 1) / 512));
    EXPECT_TRUE(fdc.dirty(0));
}

TEST_F(Fdc765Test, FormatPlacesSectorsByTheirIdsAndSkipsOnesOffTheImage) {
    auto img = MakeImage(40, 2, 9);
    fdc.mount(1, img.data(), img.size());
    PowerOnMotorAndSelect(1);
    const uint8_t cmd[] = {0x4D, 0x01, 0x02, 0x05, 0x50, 0xF6};
    for (uint8_t b : cmd) fdc.out(0x3F5, b);
    uint64_t c = 0;
    for (; c < 10'000'000 && !fdc.transfer_ready(); c += 100) fdc.tick(c);
    EXPECT_NEAR(double(c), 8e6 / 5.0, 200.0);
    uint8_t* ids = fdc.transfer_image_ptr();
    const uint8_t table[] = {0, 0, 1, 3,  0, 0, 10, 2,  40, 0, 3, 2,  0, 0, 2, 2,  7, 1, 4, 2};
    std::memcpy(ids, table, sizeof table);
    fdc.finish_transfer(sizeof table);
    const auto& d = fdc.drives[1];
    EXPECT_EQ(d.image[std::size_t(d.offset_for(0, 0, 1))], 0x00);
    EXPECT_EQ(d.image[std::size_t(d.offset_for(0, 0, 2))], 0xF6);
    EXPECT_EQ(d.image[std::size_t(d.offset_for(0, 0, 3))], 0x02);
    EXPECT_EQ(d.image[std::size_t(d.offset_for(7, 1, 4))], 0xF6);
}

TEST_F(Fdc765Test, DataRateIsLatchedAndResetsTo500Kbps) {
    EXPECT_EQ(fdc.data_rate(), 0);
    fdc.out(0x3F7, 0xFE);
    EXPECT_EQ(fdc.data_rate(), 2);
    fdc.reset();
    EXPECT_EQ(fdc.data_rate(), 0);
}

TEST_F(Fdc765Test, DorBitThreeHoldsIntWithoutClearingIt) {
    fdc.out(0x3F2, 0x00);
    fdc.out(0x3F2, 0x14);
    EXPECT_FALSE(fdc.irq_pending());
    fdc.out(0x3F2, 0x1C);
    EXPECT_TRUE(fdc.irq_pending());
    fdc.out(0x3F5, 0x08);
    fdc.in(0x3F5);
    EXPECT_FALSE(fdc.irq_pending());
}

TEST_F(Fdc765Test, SeekTakesOneStepTimePerCylinderAndReportsTheNewCylinder) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x0F); fdc.out(0x3F5, 0x00); fdc.out(0x3F5, 10);  // SEEK drive 0, cylinder 10
    EXPECT_EQ(fdc.in(0x3F4) & 0x01, 0x01) << "drive 0 busy seeking";
    EXPECT_EQ(fdc.in(0x3F4) & 0x80, 0x80) << "RQM stays up during a seek";

    uint64_t c = 0;
    for (; c < 10'000'000 && !fdc.irq_pending(); c += 100) fdc.tick(c);
    ASSERT_TRUE(fdc.irq_pending());
    EXPECT_NEAR(double(c), 10 * 0.003 * 8e6, 200.0);
    EXPECT_EQ(fdc.in(0x3F4) & 0x01, 0x00);

    fdc.out(0x3F5, 0x08);
    EXPECT_EQ(fdc.in(0x3F5), 0x20);  // ST0: seek end, drive 0
    EXPECT_EQ(fdc.in(0x3F5), 10);    // PCN
    EXPECT_FALSE(fdc.irq_pending());
}

TEST_F(Fdc765Test, SenseDriveStatusShowsTrackZeroOnlyAtCylinderZero) {
    auto img = MakeImage(40, 2, 9);
    fdc.mount(1, img.data(), img.size());
    PowerOnMotorAndSelect(1);
    fdc.out(0x3F5, 0x04); fdc.out(0x3F5, 0x01);
    EXPECT_EQ(fdc.in(0x3F5) & 0x33, 0x31);  // ST3: ready, track 0, drive 1

    fdc.out(0x3F5, 0x0F); fdc.out(0x3F5, 0x01); fdc.out(0x3F5, 3);
    fdc.out(0x3F5, 0x04); fdc.out(0x3F5, 0x01);
    EXPECT_EQ(fdc.in(0x3F5) & 0x33, 0x21);
}

TEST_F(Fdc765Test, ReadIdReportsTheCylinderUnderTheHead) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x0F); fdc.out(0x3F5, 0x00); fdc.out(0x3F5, 7);
    fdc.out(0x3F5, 0x4A); fdc.out(0x3F5, 0x04);  // READ ID, MFM, head 1
    uint8_t res[7];
    for (auto &b : res) b = fdc.in(0x3F5);
    EXPECT_EQ(res[0], 0x00);
    EXPECT_EQ(res[3], 7);  // C
    EXPECT_EQ(res[4], 1);  // H
    EXPECT_EQ(res[6], 2);  // N: 512 bytes
    EXPECT_EQ(fdc.in(0x3F4) & 0x10, 0x00) << "result phase fully drained";
}

TEST_F(Fdc765Test, MultiSectorReadRunsFromRToEotOnTheSelectedHead) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    const uint8_t cmd[] = {0xE6, 0x04, 0x00, 0x01, 0x02, 0x02, 0x04, 0x1B, 0xFF};  // C0 H1 R2-4
    for (uint8_t b : cmd) fdc.out(0x3F5, b);
    ASSERT_EQ(fdc.transfer_length(), std::size_t(3 * 512));
    for (uint64_t c = 0; c < 10'000'000 && !fdc.transfer_ready(); c += 100) fdc.tick(c);
    ASSERT_TRUE(fdc.transfer_ready());
    const uint8_t* p = fdc.transfer_image_ptr();
    EXPECT_EQ(p[0], 16);    // C0 H1 R2 is the 17th sector on the image
    EXPECT_EQ(p[1024], 18);
    fdc.finish_transfer(3 * 512);
    uint8_t res[7];
    for (auto &b : res) b = fdc.in(0x3F5);
    EXPECT_EQ(res[3], 0);
    EXPECT_EQ(res[4], 1);
    EXPECT_EQ(res[5], 4);
}

TEST_F(Fdc765Test, A360KDiskReadsAtHalfTheRateOfA12MDisk) {
    auto cycles_for_one_sector = [this](std::size_t cyl, int spt) {
        fdc.reset();
        auto img = MakeImage(cyl, 2, spt);
        fdc.mount(0, img.data(), img.size());
        PowerOnMotorAndSelect(0);
        const uint8_t cmd[] = {0xE6, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x1B, 0xFF};
        for (uint8_t b : cmd) fdc.out(0x3F5, b);
        uint64_t c = 0;
        for (; c < 10'000'000 && !fdc.transfer_ready(); c += 16) fdc.tick(c);
        return double(c);
    };
    EXPECT_NEAR(cycles_for_one_sector(80, 15), 512.0 / 62500.0 * 8e6, 32.0);  // 500 kbit/s
    EXPECT_NEAR(cycles_for_one_sector(40, 9), 512.0 / 31250.0 * 8e6, 32.0);   // 250 kbit/s
}

TEST_F(Fdc765Test, ResetDuringATransferAbandonsIt) {
    auto img = MakeImage(80, 2, 15);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    const uint8_t cmd[] = {0xE6, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x1B, 0xFF};
    for (uint8_t b : cmd) fdc.out(0x3F5, b);
    EXPECT_EQ(fdc.in(0x3F4) & 0x80, 0x00) << "no RQM in the execution phase";

    fdc.out(0x3F2, 0x18);
    for (uint64_t c = 0; c < 1'000'000; c += 100) fdc.tick(c);
    EXPECT_FALSE(fdc.transfer_ready());
    EXPECT_EQ(fdc.in(0x3F4), 0x80);
}

}  // namespace
