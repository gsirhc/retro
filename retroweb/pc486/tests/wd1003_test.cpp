// WD1003-WA2 hard disk controller: BIOS detection sequence, IDENTIFY, paced PIO
// Geometry is the WD Caviar AC2250 (1010/9/55, 256MB), not the AT's ST-4038.

#include <gtest/gtest.h>

#include "wd1003.h"

#include <string>
#include <utility>
#include <vector>

namespace {

using pc486::Wd1003;

constexpr int kCylinders = 1010;
constexpr int kHeads = 9;
constexpr int kSectorsPerTrack = 55;
constexpr long kCapacitySectors = long(kCylinders) * kHeads * kSectorsPerTrack;  // 499,950

// truncated prefix of the 256MB image; the controller bounds-checks against the mounted size
std::vector<uint8_t> MakeImage(long sectors) {
    std::vector<uint8_t> img(std::size_t(sectors) * 512, 0);
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
    // BIOS detection per rombios.c ata_detect(): scratch pattern in Sector Count/Number
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
    // soft reset leaves the ATA signature: count/number 1, cylinder 0 (ATAPI is 14h/EBh)
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    hdd.out(0x3F6, 0x04);  // assert SRST
    hdd.out(0x3F6, 0x00);
    EXPECT_EQ(hdd.in(0x1F2), 0x01);
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);  // cylinder low
    EXPECT_EQ(hdd.in(0x1F5), 0x00);  // cylinder high
    EXPECT_NE(hdd.in(0x3F6) /* alt status, doesn't clear IRQ */, 0x00);
}

TEST_F(Wd1003Test, SoftResetSignalsAbsenceForTheSlaveViaFloatedCylinderRegisters) {
    // ata_detect() tells a device from none by the cylinder signature after SRST:
    // 00h/00h present, FFh/FFh absent (bus pull-ups). Sector Count/Number read 1/1 either way.
    hdd.out(0x1F6, 0xB0);
    hdd.out(0x3F6, 0x04);
    hdd.out(0x3F6, 0x00);
    EXPECT_EQ(hdd.in(0x1F2), 0x01);  // count/number still 1/1
    EXPECT_EQ(hdd.in(0x1F3), 0x01);
    EXPECT_EQ(hdd.in(0x1F4), 0xFF);  // cylinder low/high float
    EXPECT_EQ(hdd.in(0x1F5), 0xFF);

    // the master keeps its 00h/00h signature after a slave-selected reset
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x3F6, 0x04);
    hdd.out(0x3F6, 0x00);
    EXPECT_EQ(hdd.in(0x1F4), 0x00);
    EXPECT_EQ(hdd.in(0x1F5), 0x00);
}

TEST_F(Wd1003Test, FloatedSignatureIsATwoReadWindowNotAPersistentState) {
    // the float lasts only the two cylinder reads ata_detect() makes; the boot loader
    // reads Status later with the slave still selected and needs the master's value
    hdd.out(0x1F6, 0xB0);
    hdd.out(0x3F6, 0x04);
    hdd.out(0x3F6, 0x00);  // arms the two-read float window
    // Status is not part of the float window
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY

    hdd.in(0x1F4);  // consume both floating reads
    hdd.in(0x1F5);
    // with the window spent, cylinder reads return the master's values
    EXPECT_EQ(hdd.in(0x1F4), 0x00);
    EXPECT_EQ(hdd.in(0x1F5), 0x00);
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);
}

TEST_F(Wd1003Test, IdentifyDeviceReportsTheCaviarAc2250Geometry) {
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);  // IDENTIFY DEVICE
    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);  // DRQ: data ready
    std::vector<uint16_t> w;
    for (int i = 0; i < 256; ++i) w.push_back(hdd.data_in16());
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // DRQ cleared once fully drained

    // WD translated setup geometry for the AC2250
    EXPECT_EQ(w[1], kCylinders);
    EXPECT_EQ(w[3], kHeads);
    EXPECT_EQ(w[6], kSectorsPerTrack);
    EXPECT_EQ(w[1], Wd1003::kCylinders);
    EXPECT_EQ(w[3], Wd1003::kHeads);
    EXPECT_EQ(w[6], Wd1003::kSectorsPerTrack);

    // Model number, words 27-46, byte-swapped within each word.
    std::string model;
    for (int i = 27; i < 47; ++i) {
        model += char(w[i] >> 8);
        model += char(w[i] & 0xFF);
    }
    EXPECT_EQ(model.substr(0, 10), "WDC AC2250");

    // rombios.c reads word 5 to size its PIO transfer chunks
    EXPECT_EQ(w[5], 512);

    // words 60/61: 499,950 sectors x 512 = 255,974,400 bytes
    uint32_t total = uint32_t(w[60]) | (uint32_t(w[61]) << 16);
    EXPECT_EQ(total, uint32_t(kCapacitySectors));
    EXPECT_EQ(uint64_t(total) * 512, 255974400u);
    EXPECT_EQ(uint64_t(total) * 512, Wd1003::kImageBytes);
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
    // alternate status read does not clear the pending interrupt
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ: data ready
    EXPECT_TRUE(hdd.irq_pending());

    EXPECT_EQ(hdd.data_in16() & 0xFF, 2);
}

TEST_F(Wd1003Test, ReadSectorsUsesLbaAddressingWhenDriveHeadBitSixIsSet) {
    // the BIOS always uses 28-bit LBA (Drive/Head bit 6); LBA 0 coincides with CHS(0,0,1)
    MountDrive0();

    // LBA 100 would be out of range as CHS (55 sectors/track)
    hdd.out(0x1F6, 0xA0 | 0x40);  // drive 0, LBA mode (bit6), LBA[27:24]=0
    hdd.out(0x1F2, 1);            // 1 sector
    hdd.out(0x1F3, 100);          // LBA[7:0] = 100
    hdd.out(0x1F4, 0);            // LBA[15:8] = 0
    hdd.out(0x1F5, 0);            // LBA[23:16] = 0
    hdd.out(0x1F7, 0x20);         // READ SECTORS

    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);  // DRQ, not IDNF
    EXPECT_EQ(hdd.data_in16() & 0xFF, 100);
}

TEST_F(Wd1003Test, WriteSectorsAcceptsDataImmediatelyThenCommitsSynchronously) {
    // writes commit on the last byte with no tick(); rombios.c ata_cmd_data_io() never waits for write BSY
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 1);
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    hdd.out(0x1F7, 0x30);  // WRITE SECTORS

    ASSERT_TRUE(hdd.in(0x1F7) & 0x08);  // DRQ immediately
    for (int i = 0; i < 255; ++i) hdd.data_out16(0xABCD);
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);  // still mid-transfer
    hdd.data_out16(0xABCD);  // last word completes the sector
    EXPECT_TRUE(hdd.in(0x3F6) & 0x40);  // DRDY, synchronously
    EXPECT_FALSE(hdd.in(0x3F6) & 0x80);  // not BSY
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.dirty(0));

    // one 4KB dirty page covers the 512 bytes touched
    auto ranges = hdd.dirty_ranges(0);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].offset, 0u);
    EXPECT_EQ(ranges[0].length, 4096u);
}

TEST_F(Wd1003Test, DirtyRangesTracksOnlyWhatWasActuallyWrittenAndClearsCleanly) {
    // persistHddIfDirty() copies only dirty ranges; distant writes must stay separate ranges
    MountDrive0(4096);  // 512 dirty pages
    EXPECT_TRUE(hdd.dirty_ranges(0).empty());

    auto write_one_sector = [&](uint16_t lba_low_sector_number, uint8_t cyl_low, uint8_t cyl_high) {
        hdd.out(0x1F6, 0xA0);
        hdd.out(0x1F2, 1);  // one sector
        hdd.out(0x1F3, uint8_t(lba_low_sector_number));
        hdd.out(0x1F4, cyl_low); hdd.out(0x1F5, cyl_high);
        hdd.out(0x1F7, 0x30);  // WRITE SECTORS
        for (int i = 0; i < 256; ++i) hdd.data_out16(0x1234);
    };

    write_one_sector(1, 0, 0);
    // sector 17 is byte 8192, page 2, so page 1 stays clean
    write_one_sector(17, 0, 0);

    auto ranges = hdd.dirty_ranges(0);
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].offset, 0u);
    EXPECT_EQ(ranges[0].length, 4096u);
    EXPECT_EQ(ranges[1].offset, 8192u);
    EXPECT_EQ(ranges[1].length, 4096u);

    hdd.clear_dirty(0);
    EXPECT_FALSE(hdd.dirty(0));
    EXPECT_TRUE(hdd.dirty_ranges(0).empty());
}

TEST_F(Wd1003Test, RecalibrateAndInitializeDeviceParametersCompleteImmediately) {
    hdd.out(0x1F7, 0x10);  // RECALIBRATE
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x40);  // DRDY

    hdd.out(0x1F2, 55);           // sectors per track
    hdd.out(0x1F6, 0xA0 | 0x08);  // heads-1 = 8 (9 heads)
    hdd.out(0x1F7, 0x91);  // INITIALIZE DEVICE PARAMETERS
    EXPECT_TRUE(hdd.irq_pending());
}

TEST_F(Wd1003Test, AbsentSlaveDriveOnlyRefusesToExecuteCommands) {
    // drive 1 is absent: only command writes (0x1F7) are gated by drive select, so no phantom
    // IDENTIFY runs. Other task-file registers are shared latches, never gated; the BIOS
    // programs them before reselecting the master.
    MountDrive0();

    hdd.out(0x1F6, 0xB0);  // slave, never mounted
    hdd.out(0x1F2, 0x55);
    hdd.out(0x1F3, 0xAA);
    EXPECT_EQ(hdd.in(0x1F2), 0x55);
    EXPECT_EQ(hdd.in(0x1F3), 0xAA);

    hdd.out(0x1F7, 0xEC);  // IDENTIFY to the absent drive
    EXPECT_FALSE(hdd.irq_pending());
    EXPECT_FALSE(hdd.in(0x1F7) & 0x08);  // no IDENTIFY data

    // switching to drive 0 runs the same command
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F7, 0xEC);
    EXPECT_TRUE(hdd.irq_pending());
    EXPECT_TRUE(hdd.in(0x1F7) & 0x08);
}

TEST_F(Wd1003Test, ReadPastTheEndOfTheImageReportsIdNotFound) {
    MountDrive0(64);  // only 64 sectors of image behind a 256MB geometry
    hdd.out(0x1F6, 0xA0 | 0x40);  // LBA mode
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 200);          // LBA 200: inside the geometry, past the image
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    hdd.out(0x1F7, 0x20);
    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    EXPECT_TRUE(hdd.in(0x3F6) & 0x01);   // ERR
    EXPECT_EQ(hdd.in(0x1F1), 0x10);      // IDNF
    EXPECT_FALSE(hdd.in(0x3F6) & 0x08);
}

TEST_F(Wd1003Test, BusyReflectsAnInFlightTransfer) {
    MountDrive0();
    hdd.out(0x1F6, 0xA0);
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 3);
    hdd.out(0x1F4, 0); hdd.out(0x1F5, 0);
    EXPECT_FALSE(hdd.busy());
    hdd.out(0x1F7, 0x20);      // READ SECTORS
    EXPECT_TRUE(hdd.busy());
    for (uint64_t c = 0; c < 10'000'000 && hdd.busy(); c += 100) hdd.tick(c);
    EXPECT_FALSE(hdd.busy());
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

TEST_F(Wd1003Test, MountByMoveTakesTheImageWithoutCopying) {
    auto img = MakeImage(64);
    const uint8_t* data = img.data();
    hdd.mount(0, std::move(img));
    EXPECT_EQ(hdd.drives[0].image.data(), data);
    EXPECT_EQ(hdd.drives[0].image.size(), std::size_t(64) * 512);
    EXPECT_TRUE(hdd.drives[0].present);
    EXPECT_FALSE(hdd.dirty(0));
    EXPECT_EQ(hdd.drives[0].dirty_page.size(), std::size_t(64) * 512 / Wd1003::Drive::kDirtyPageSize);
    EXPECT_EQ(hdd.drives[0].cylinders, kCylinders);

    hdd.out(0x1F6, 0xA0 | 0x40);  // LBA mode
    hdd.out(0x1F2, 1);
    hdd.out(0x1F3, 5);
    hdd.out(0x1F4, 0);
    hdd.out(0x1F5, 0);
    hdd.out(0x1F7, 0x20);  // READ SECTORS
    for (uint64_t c = 0; c < 10'000'000 && (hdd.in(0x3F6) & 0x80); c += 100) hdd.tick(c);
    ASSERT_TRUE(hdd.in(0x3F6) & 0x08);
    EXPECT_EQ(hdd.data_in16() & 0xFF, 5);
}

}  // namespace
