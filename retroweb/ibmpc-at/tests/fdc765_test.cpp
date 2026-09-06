// GoogleTest suite for the NEC uPD765 floppy controller: command/parameter
// sequencing, the Main Status Register's phase bits, SEEK/RECALIBRATE
// paced completion via SENSE INTERRUPT STATUS, and the transfer_ready()/
// transfer_image_ptr()/finish_transfer() handoff chipset.cpp uses to
// perform the actual memory<->image copy.

#include <gtest/gtest.h>

#include "fdc765.h"

#include <cstring>
#include <vector>

namespace {

using ibmpcat::Fdc765;

std::vector<uint8_t> MakeImage(std::size_t cyl, int heads, int spt) {
    std::vector<uint8_t> img(cyl * heads * spt * 512, 0);
    // Stamp each sector's first byte with a recognizable, distinct pattern
    // so a test can confirm exactly which bytes landed where.
    for (std::size_t i = 0; i < img.size(); i += 512) img[i] = uint8_t((i / 512) & 0xFF);
    return img;
}

class Fdc765Test : public ::testing::Test {
protected:
    Fdc765 fdc;
    void SetUp() override { fdc.reset(); }

    void PowerOnMotorAndSelect(int drive) {
        // DOR: motor for the selected drive on, ~RESET high (normal op),
        // DMA/IRQ enable on (real BIOS always sets this for normal
        // operation -- without it, completed commands never raise IRQ6),
        // drive select. Leaving the held-reset state itself raises an
        // interrupt on real hardware (a real driver clears it via SENSE
        // INTERRUPT STATUS before doing anything else) -- acknowledge it
        // here the same way so tests can look at irq_pending() for the
        // condition they actually care about afterward.
        uint8_t bit = drive == 0 ? 0x10 : 0x20;
        fdc.out(0x3F2, uint8_t(0x04 | 0x08 | bit | drive));
        fdc.clear_irq();
    }
};

TEST_F(Fdc765Test, DoesNotOwnPortThreeF6) {
    // 0x3F6, in the middle of the FDC's otherwise-contiguous port block, is
    // genuinely NOT decoded by a real AT's floppy controller -- it belongs
    // to the hard disk controller's Device Control / Alternate Status
    // register (see wd1003.h). Chipset::io_in/io_out check fdc.owns()
    // before hdd.owns(), so a too-wide range here would silently steal the
    // port from the HDD before it was ever reached -- a real bug WD1003
    // integration testing caught. See IBM_PCAT_REVIEW.md.
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
    // Real uPD765/8272 hardware drops IRQ6 as soon as the CPU reads the
    // first result byte (ST0 in a 7-byte READ/WRITE DATA result phase) --
    // not after every result byte is drained. Some real driver code reads
    // only ST0 before moving on to other work; modeling "IRQ clears on
    // full drain" left the interrupt permanently pending and re-triggered
    // its ISR forever, an infinite-IRQ-storm bug this session actually
    // hit trying to boot real BIOS + FreeDOS. See IBM_PCAT_REVIEW.md §8.
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
    // Real hardware: DSKCHG asserts whenever media is swapped and only
    // clears once the drive actually steps (RECALIBRATE/SEEK) afterward --
    // software that copies files across floppy swaps (a multi-disk
    // installer, say) polls this specifically to confirm the user really
    // swapped media before trusting a re-read. A controller that always
    // reports "unchanged" leaves that software waiting forever. See
    // IBM_PCAT_REVIEW.md.
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

}  // namespace
