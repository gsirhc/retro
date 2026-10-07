// uPD765 floppy controller: command sequencing, MSR phases, SEEK/RECALIBRATE, transfer handoff

#include <gtest/gtest.h>

#include "fdc765.h"

#include <cstring>
#include <vector>

namespace {

using pc486::Fdc765;

std::vector<uint8_t> MakeImage(std::size_t cyl, int heads, int spt) {
    std::vector<uint8_t> img(cyl * heads * spt * 512, 0);
    for (std::size_t i = 0; i < img.size(); i += 512) img[i] = uint8_t((i / 512) & 0xFF);
    return img;
}

class Fdc765Test : public ::testing::Test {
protected:
    Fdc765 fdc;
    void SetUp() override { fdc.reset(); }

    void PowerOnMotorAndSelect(int drive) {
        // DOR: motor on, ~RESET high, DMA/IRQ enable, drive select. Ack the reset interrupt like a real driver.
        uint8_t bit = drive == 0 ? 0x10 : 0x20;
        fdc.out(0x3F2, uint8_t(0x04 | 0x08 | bit | drive));
        fdc.clear_irq();
    }
};

TEST_F(Fdc765Test, DoesNotOwnPortThreeF6) {
    // port 0x3F6 belongs to the HDD (wd1003.h); fdc.owns() is checked first
    EXPECT_FALSE(fdc.owns(0x3F6));
    EXPECT_TRUE(fdc.owns(0x3F5));
    EXPECT_TRUE(fdc.owns(0x3F7));
}

TEST_F(Fdc765Test, SpecifyIsAcceptedWithoutResult) {
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x03);  // SPECIFY
    fdc.out(0x3F5, 0xDF);  // SRT/HUT
    fdc.out(0x3F5, 0x02);  // HLT/ND
    EXPECT_EQ(fdc.in(0x3F4) & 0x10, 0x00);  // command done
    EXPECT_EQ(fdc.in(0x3F4) & 0x80, 0x80);  // RQM
}

TEST_F(Fdc765Test, RecalibrateCompletesAfterPacedDelayAndReportsViaSenseInterrupt) {
    auto img = MakeImage(80, 2, 18);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x07);  // RECALIBRATE
    fdc.out(0x3F5, 0x00);  // drive 0

    EXPECT_FALSE(fdc.irq_pending());
    for (int i = 0; i < 1000 && !fdc.irq_pending(); ++i) fdc.tick(uint64_t(i) * 1000);
    EXPECT_TRUE(fdc.irq_pending());

    fdc.out(0x3F5, 0x08);  // SENSE INTERRUPT STATUS
    EXPECT_EQ(fdc.in(0x3F4) & 0xC0, 0xC0);  // RQM+DIO: result byte ready
    uint8_t st0 = fdc.in(0x3F5);
    uint8_t pcn = fdc.in(0x3F5);
    EXPECT_EQ(st0 & 0x20, 0x20);  // seek-end
    EXPECT_EQ(pcn, 0);  // recalibrated to cylinder 0
}

TEST_F(Fdc765Test, SenseInterruptWithNothingPendingReportsInvalidCommand) {
    fdc.out(0x3F5, 0x08);
    EXPECT_EQ(fdc.in(0x3F5), 0x80);
}

TEST_F(Fdc765Test, ReadDataTransfersTheRequestedSectorAfterPacing) {
    auto img = MakeImage(80, 2, 18);
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
    // sector 3 is the 3rd 512-byte block, stamped index 2
    EXPECT_EQ(ptr[0], 2);

    fdc.finish_transfer(512);
    EXPECT_EQ(fdc.in(0x3F4) & 0xC0, 0xC0);  // result phase
    uint8_t st0 = fdc.in(0x3F5);
    EXPECT_EQ(st0, 0x00);  // normal termination
}

TEST_F(Fdc765Test, WriteDataMarksDriveDirty) {
    // 720KB media in the 1.44MB bay, drive 0
    auto img = MakeImage(80, 2, 9);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    ASSERT_FALSE(fdc.dirty(0));

    fdc.out(0x3F5, 0xC5);  // WRITE DATA (MT|MFM|base 0x05)
    fdc.out(0x3F5, 0x00);  // drive 0, head 0
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
    ptr[0] = 0xAB;
    fdc.finish_transfer(512);
    EXPECT_TRUE(fdc.dirty(0));
}

TEST_F(Fdc765Test, InterruptClearsOnFirstResultByteNotTheWholePhase) {
    // IRQ6 drops on the first result byte read, not on full drain
    auto img = MakeImage(80, 2, 18);
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

    fdc.in(0x3F5);  // only ST0 read
    EXPECT_FALSE(fdc.irq_pending());
    EXPECT_TRUE(fdc.in(0x3F4) & 0x10);  // still busy
}

TEST_F(Fdc765Test, DiskChangeLineSetByMountAndClearedBySeek) {
    // DSKCHG stays set until the drive steps after a media swap
    auto img = MakeImage(80, 2, 18);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80);

    fdc.out(0x3F5, 0x07); fdc.out(0x3F5, 0x00);  // RECALIBRATE drive 0
    EXPECT_FALSE(fdc.in(0x3F7) & 0x80);

    // swapping again re-asserts it
    auto img2 = MakeImage(80, 2, 15);
    fdc.mount(0, img2.data(), img2.size());
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80);
}

TEST_F(Fdc765Test, EjectingADiskAssertsDiskChangeAndAnEmptyBayKeepsIt) {
    auto img = MakeImage(80, 2, 18);
    fdc.mount(0, img.data(), img.size());
    PowerOnMotorAndSelect(0);
    fdc.out(0x3F5, 0x07); fdc.out(0x3F5, 0x00);  // RECALIBRATE drive 0
    ASSERT_FALSE(fdc.in(0x3F7) & 0x80);

    fdc.unmount(0);
    EXPECT_FALSE(fdc.mounted(0));
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80) << "ejecting is a media change";

    fdc.out(0x3F5, 0x07); fdc.out(0x3F5, 0x00);
    EXPECT_TRUE(fdc.in(0x3F7) & 0x80) << "a step with no disk in the drive does not reset DSKCHG";

    fdc.mount(0, img.data(), img.size());
    fdc.out(0x3F5, 0x07); fdc.out(0x3F5, 0x00);
    EXPECT_FALSE(fdc.in(0x3F7) & 0x80);
}

TEST_F(Fdc765Test, UnmountingTheOtherDriveLeavesThisOneAlone) {
    auto img = MakeImage(80, 2, 18);
    fdc.mount(0, img.data(), img.size());
    fdc.unmount(1);
    EXPECT_TRUE(fdc.mounted(0));
    EXPECT_FALSE(fdc.mounted(1));
}

TEST_F(Fdc765Test, MountedMediaSurvivesControllerReset) {
    auto img = MakeImage(80, 2, 18);
    fdc.mount(0, img.data(), img.size());
    fdc.reset();
    EXPECT_TRUE(fdc.mounted(0));
}

}  // namespace
