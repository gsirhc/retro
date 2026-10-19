// GoogleTest suite for the MITS 88-DCDD (disk88.{h,cpp}), driven the way the boot
// PROM and CP/M BIOS do. Status reads back inverted (0 = true). Expected values
// come from the MITS 88-DCDD manual pp.133-136 and the Pertec FD-400 manual.

#include <gtest/gtest.h>

#include "disk88.h"

#include <cstdint>
#include <numeric>
#include <vector>

using altair::Disk88;

namespace {

constexpr uint8_t SEL   = 0x08;
constexpr uint8_t CTL   = 0x09;   // OUT: function   IN: sector position
constexpr uint8_t DATA  = 0x0A;

constexpr uint8_t FN_STEP_IN   = 0x01;
constexpr uint8_t FN_STEP_OUT  = 0x02;
constexpr uint8_t FN_HEAD_LOAD = 0x04;
constexpr uint8_t FN_WRITE     = 0x80;

constexpr uint8_t F_ENWD = 0x01;
constexpr uint8_t F_MOVE = 0x02;
constexpr uint8_t F_HEAD = 0x04;
constexpr uint8_t F_INTE = 0x20;
constexpr uint8_t F_TRK0 = 0x40;
constexpr uint8_t F_NRDA = 0x80;

constexpr uint64_t kMs = 2000;              // CPU cycles at 2 MHz
constexpr double   kSectorCycles = 2e6 / (360.0 / 60.0 * 32.0);

// every byte encodes its own (track, sector, offset)
std::vector<uint8_t> makeImage() {
    std::vector<uint8_t> img(Disk88::kImageSize);
    for (int t = 0; t < Disk88::kTracks; ++t)
        for (int s = 0; s < Disk88::kSectors; ++s)
            for (int b = 0; b < Disk88::kSectorLen; ++b)
                img[(t * Disk88::kSectors + s) * Disk88::kSectorLen + b] =
                    static_cast<uint8_t>((t * 7 + s * 3 + b) & 0xFF);
    return img;
}

std::size_t offsetOf(int track, int sector) {
    return (std::size_t(track) * Disk88::kSectors + sector) * Disk88::kSectorLen;
}

// sync to `sector` via IN 0x09, then read 137 bytes from the data port
std::vector<uint8_t> readSector(Disk88 &d, int sector) {
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t pos = d.in(CTL);
        if (((pos >> 1) & 0x1F) == sector) break;
    }
    std::vector<uint8_t> out;
    for (int i = 0; i < Disk88::kSectorLen; ++i) out.push_back(d.in(DATA));
    return out;
}

class DiskTest : public ::testing::Test {
protected:
    Disk88 d;
    std::vector<uint8_t> img = makeImage();

    void selectAndLoad(int drive = 0) {
        d.out(SEL, static_cast<uint8_t>(drive));
        d.out(CTL, FN_HEAD_LOAD);
    }
};

TEST_F(DiskTest, MountAndPresence) {
    EXPECT_FALSE(d.mounted(0));
    EXPECT_TRUE(d.mount(0, img.data(), img.size()));
    EXPECT_TRUE(d.mounted(0));
    EXPECT_FALSE(d.mount(-1, img.data(), img.size()));
    EXPECT_FALSE(d.mount(Disk88::kDrives, img.data(), img.size()));
    d.unmount(0);
    EXPECT_FALSE(d.mounted(0));
}

// nothing drives the bus until the controller is enabled (p.134: "all false")
TEST_F(DiskTest, DisabledControllerFloatsAllPorts) {
    d.mount(0, img.data(), img.size());
    EXPECT_EQ(d.in(SEL), 0xFF);
    EXPECT_EQ(d.in(CTL), 0xFF);
    EXPECT_EQ(d.in(DATA), 0xFF);
    d.out(SEL, 0x00);
    EXPECT_NE(d.in(SEL), 0xFF);
    d.out(SEL, 0x80);
    EXPECT_EQ(d.in(SEL), 0xFF);
    EXPECT_EQ(d.selectedDrive(), -1);
}

// p.133 note a: with the door open the drive and controller can't be enabled
TEST_F(DiskTest, EmptyDriveCannotBeEnabled) {
    d.out(SEL, 0x00);
    EXPECT_EQ(d.selectedDrive(), -1);
    d.out(CTL, FN_HEAD_LOAD);
    EXPECT_FALSE(d.headLoaded());
    EXPECT_EQ(d.in(SEL), 0xFF);
    EXPECT_EQ(d.in(CTL), 0xFF);
    EXPECT_EQ(d.in(DATA), 0xFF);
}

TEST_F(DiskTest, OpeningTheSelectedDriveClearsTheController) {
    d.mount(0, img.data(), img.size());
    selectAndLoad();
    ASSERT_TRUE(d.headLoaded());
    d.unmount(0);
    EXPECT_EQ(d.selectedDrive(), -1);
    EXPECT_EQ(d.in(SEL), 0xFF);
}

// p.134: D3 and D4 are not used and read 0; D5 is the bus INTE, inverted
TEST_F(DiskTest, StatusUnusedBitsReadZeroAndInteFollowsTheBus) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    EXPECT_EQ(d.in(SEL) & 0x18, 0x00);
    EXPECT_TRUE(d.in(SEL) & F_INTE);
    d.setInte(true);
    EXPECT_FALSE(d.in(SEL) & F_INTE);
}

TEST_F(DiskTest, HeadStepsClampAtBothEnds) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    for (int i = 0; i < 200; ++i) d.out(CTL, FN_STEP_IN);
    EXPECT_EQ(d.track(0), 76);
    for (int i = 0; i < 200; ++i) d.out(CTL, FN_STEP_OUT);
    EXPECT_EQ(d.track(0), 0);
}

// bus RESET sends no STEP pulses, so the head stays put
TEST_F(DiskTest, ResetDoesNotHomeTheHead) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    for (int i = 0; i < 5; ++i) d.out(CTL, FN_STEP_IN);
    ASSERT_EQ(d.track(0), 5);

    d.reset();
    EXPECT_EQ(d.track(0), 5);
    EXPECT_EQ(d.in(SEL), 0xFF);
}

// FD-400 §2.6.3: the track 0 line is a function of head position
TEST_F(DiskTest, Track0LineFollowsHead) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    auto atTrack0 = [&] { return (d.in(SEL) & F_TRK0) == 0; };
    EXPECT_TRUE(atTrack0());
    d.out(CTL, FN_STEP_IN);
    EXPECT_EQ(d.track(0), 1);
    EXPECT_FALSE(atTrack0());
    d.out(CTL, FN_STEP_OUT);
    EXPECT_TRUE(atTrack0());
}

// p.136: only SR0..SR5 are driven, so D6/D7 float high
TEST_F(DiskTest, SectorPositionNeedsTheHeadAndCyclesModulo32) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    EXPECT_EQ(d.in(CTL), 0xFF);
    d.out(CTL, FN_HEAD_LOAD);
    int seen = 0;
    for (int i = 0; i < 32; ++i) {
        uint8_t v = d.in(CTL);
        EXPECT_EQ(v & 0xC0, 0xC0);
        EXPECT_EQ(v & 0x01, 0x00);
        seen |= 1 << ((v >> 1) & 0x1F);
    }
    EXPECT_EQ(seen, -1 & 0xFFFFFFFF);
}

TEST_F(DiskTest, SectorReadMatchesImage) {
    d.mount(0, img.data(), img.size());
    for (int track : {0, 1, 5, 40, 76}) {
        d.out(SEL, 0x00);
        d.out(CTL, FN_HEAD_LOAD);
        int cur = d.track(0);
        for (; cur < track; ++cur) d.out(CTL, FN_STEP_IN);
        for (; cur > track; --cur) d.out(CTL, FN_STEP_OUT);
        for (int sector : {0, 7, 31}) {
            auto got = readSector(d, sector);
            const std::size_t base = offsetOf(track, sector);
            for (int b = 0; b < Disk88::kSectorLen; ++b)
                ASSERT_EQ(got[b], img[base + b])
                    << "track " << track << " sector " << sector << " byte " << b;
        }
    }
}

// p.135: the 138th byte written is 000 and fills the rest of the sector
TEST_F(DiskTest, ReadsPastByte137ReturnTheZeroTail) {
    std::vector<uint8_t> ab(Disk88::kImageSize, 0xAB);
    d.mount(0, ab.data(), ab.size());
    selectAndLoad();
    auto first137 = readSector(d, 3);
    EXPECT_EQ(first137.back(), 0xAB);
    EXPECT_EQ(d.in(DATA), 0x00);
    EXPECT_EQ(d.in(DATA), 0x00);
}

TEST_F(DiskTest, WriteSequenceRoundTrips) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    d.out(CTL, FN_HEAD_LOAD);
    for (int i = 0; i < 3; ++i) d.out(CTL, FN_STEP_IN);

    for (int g = 0; g < 64; ++g)
        if (((d.in(CTL) >> 1) & 0x1F) == 4) break;
    d.out(CTL, FN_WRITE);
    EXPECT_FALSE(d.in(SEL) & F_ENWD);
    std::vector<uint8_t> payload(Disk88::kSectorLen);
    std::iota(payload.begin(), payload.end(), 0x11);
    for (uint8_t v : payload) d.out(DATA, v);
    d.out(DATA, 0x00);

    EXPECT_TRUE(d.dirty(0));
    const std::size_t base = offsetOf(3, 4);
    for (int b = 0; b < Disk88::kSectorLen; ++b)
        EXPECT_EQ(d.image(0)[base + b], payload[b]) << "byte " << b;

    d.out(CTL, FN_STEP_OUT); d.out(CTL, FN_STEP_IN);
    auto got = readSector(d, 4);
    EXPECT_EQ(got, payload);

    d.clearDirty(0);
    EXPECT_FALSE(d.dirty(0));
}

// p.136 note a: the write circuit repeats the last byte to the end of the sector
TEST_F(DiskTest, ShortWriteRepeatsTheLastByte) {
    d.mount(0, img.data(), img.size());
    selectAndLoad();
    for (int g = 0; g < 64; ++g)
        if (((d.in(CTL) >> 1) & 0x1F) == 9) break;
    d.out(CTL, FN_WRITE);
    for (int i = 0; i < 10; ++i) d.out(DATA, static_cast<uint8_t>(0x80 + i));
    (void)d.in(CTL);
    const std::size_t base = offsetOf(0, 9);
    EXPECT_EQ(d.image(0)[base + 9], 0x89);
    for (int b = 10; b < Disk88::kSectorLen; ++b)
        ASSERT_EQ(d.image(0)[base + b], 0x89) << "byte " << b;
}

TEST_F(DiskTest, ReadWithNoDiskIsHarmless) {
    d.out(SEL, 0x00);
    d.out(CTL, FN_HEAD_LOAD);
    for (int i = 0; i < 10; ++i) (void)d.in(DATA);
    SUCCEED();
}

// --- real rotation: CPU cycles at 2 MHz drive the disk clock ---

class TimedDisk : public ::testing::Test {
protected:
    Disk88 d;
    std::vector<uint8_t> img = makeImage();
    uint64_t cy = 1000;

    void SetUp() override {
        d.setSpeed(Disk88::kRealSectorsPerSec);
        d.mount(0, img.data(), img.size());
        d.tick(cy);
    }
    uint8_t in(uint8_t port) { d.clock(cy); uint8_t v = d.in(port); cy += 10; return v; }
    void out(uint8_t port, uint8_t v) { d.clock(cy); d.out(port, v); cy += 10; }
    void ready() {
        out(SEL, 0x00);
        out(CTL, FN_HEAD_LOAD);
        cy += 50 * kMs;
    }
    // poll IN 0x09 like the BIOS; returns the cycle the sector came true
    uint64_t findSector(int sector) {
        for (int g = 0; g < 1000000; ++g) {
            uint64_t at = cy;
            uint8_t v = in(CTL);
            if (!(v & 0x01) && ((v >> 1) & 0x1F) == sector) return at;
        }
        ADD_FAILURE() << "sector " << sector << " never came around";
        return cy;
    }
    uint8_t readByte() {
        for (int g = 0; g < 100000; ++g)
            if (!(in(SEL) & F_NRDA)) return in(DATA);
        ADD_FAILURE() << "NRDA never came true";
        return 0;
    }
};

// 360 rpm over 32 sectors (FD-400 Table 1-1), SR0 true for 30 us (p.136)
TEST_F(TimedDisk, SectorsComeAroundEvery5208usWith30usSectorTrue) {
    ready();
    uint64_t t0 = findSector(4);
    int truePolls = 1;
    while (!(in(CTL) & 0x01)) ++truePolls;
    EXPECT_GE(truePolls, 5);
    EXPECT_LE(truePolls, 7);
    uint64_t t1 = findSector(5);
    EXPECT_NEAR(double(t1 - t0), kSectorCycles, 12);
    while (!(in(CTL) & 0x01)) {}
    uint64_t t2 = findSector(5);
    EXPECT_NEAR(double(t2 - t1), 32 * kSectorCycles, 12);
}

// p.134: HS true 40 ms after head load, and it gates the sector position port
TEST_F(TimedDisk, HeadStatusSettles40msAfterLoad) {
    out(SEL, 0x00);
    uint64_t t0 = cy;
    out(CTL, FN_HEAD_LOAD);
    EXPECT_EQ(in(CTL), 0xFF);
    cy = t0 + 39 * kMs;
    EXPECT_TRUE(in(SEL) & F_HEAD);
    EXPECT_EQ(in(CTL), 0xFF);
    cy = t0 + 41 * kMs;
    EXPECT_FALSE(in(SEL) & F_HEAD);
    EXPECT_NE(in(CTL), 0xFF);
}

// p.134: move head goes false 10 ms, true 1 ms, false 20 ms after a step
TEST_F(TimedDisk, MoveHeadWindowsAfterAStep) {
    out(SEL, 0x00);
    cy += 100 * kMs;
    EXPECT_FALSE(in(SEL) & F_MOVE);
    uint64_t t0 = cy;
    out(CTL, FN_STEP_IN);
    auto moveOkAt = [&](double ms) { cy = t0 + uint64_t(ms * kMs); return !(in(SEL) & F_MOVE); };
    EXPECT_FALSE(moveOkAt(5));
    EXPECT_FALSE(moveOkAt(9.9));
    EXPECT_TRUE(moveOkAt(10.4));
    EXPECT_FALSE(moveOkAt(11.2));
    EXPECT_FALSE(moveOkAt(30.5));
    EXPECT_TRUE(moveOkAt(31.2));
}

// FD-400 §2.6.3.1: track 0 is held off until 40 ms after the last step
TEST_F(TimedDisk, Track0ReportsOnlyAfterTheHeadSettles) {
    out(SEL, 0x00);
    out(CTL, FN_STEP_IN);
    cy += 100 * kMs;
    EXPECT_TRUE(in(SEL) & F_TRK0);
    uint64_t t0 = cy;
    out(CTL, FN_STEP_OUT);
    ASSERT_EQ(d.track(0), 0);
    cy = t0 + 39 * kMs;
    EXPECT_TRUE(in(SEL) & F_TRK0);
    cy = t0 + 41 * kMs;
    EXPECT_FALSE(in(SEL) & F_TRK0);
}

// 250 kbit/s: a byte every 32 us, first one 140 us after SR0 (p.136)
TEST_F(TimedDisk, BytesArriveEvery32usAndAReaderThatFallsBehindLosesThem) {
    ready();
    uint64_t t0 = findSector(6);
    while (in(SEL) & F_NRDA) {}
    EXPECT_GE(cy - t0, 280u);
    EXPECT_LE(cy - t0, 280u + 30);
    std::vector<uint8_t> got{in(DATA)};
    for (int i = 1; i < Disk88::kSectorLen; ++i) got.push_back(readByte());
    EXPECT_EQ(got, std::vector<uint8_t>(img.begin() + offsetOf(0, 6),
                                        img.begin() + offsetOf(0, 7)));

    findSector(8);
    uint8_t first = readByte();
    EXPECT_EQ(first, img[offsetOf(0, 8)]);
    cy += 200;
    EXPECT_EQ(in(DATA), img[offsetOf(0, 8) + 3]);
}

// p.135: ENWD first 280 us after SR0, then every 32 us; late bytes repeat
TEST_F(TimedDisk, TimedWriteFollowsEnwd) {
    ready();
    uint64_t t0 = findSector(12);
    out(CTL, FN_WRITE);
    while (in(SEL) & F_ENWD) {}
    EXPECT_GE(cy - t0, 560u);
    EXPECT_LE(cy - t0, 560u + 30);
    std::vector<uint8_t> payload(Disk88::kSectorLen);
    std::iota(payload.begin(), payload.end(), 0x40);
    for (uint8_t v : payload) {
        while (in(SEL) & F_ENWD) {}
        out(DATA, v);
    }
    while (in(SEL) & F_ENWD) {}
    out(DATA, 0x00);
    cy += 10 * kMs;
    d.tick(cy);
    EXPECT_TRUE(d.dirty(0));
    for (int b = 0; b < Disk88::kSectorLen; ++b)
        ASSERT_EQ(d.image(0)[offsetOf(0, 12) + b], payload[b]) << "byte " << b;

    findSector(13);
    out(CTL, FN_WRITE);
    while (in(SEL) & F_ENWD) {}
    out(DATA, 0x55);
    cy += 32 * 2 * 3;
    out(DATA, 0x66);
    cy += 10 * kMs;
    d.tick(cy);
    const std::size_t s13 = offsetOf(0, 13);
    EXPECT_EQ(d.image(0)[s13 + 0], 0x55);
    EXPECT_EQ(d.image(0)[s13 + 1], 0x55);
    EXPECT_EQ(d.image(0)[s13 + 2], 0x55);
    EXPECT_EQ(d.image(0)[s13 + 3], 0x66);
    EXPECT_EQ(d.image(0)[s13 + 136], 0x66);
}

TEST_F(TimedDisk, FasterSpeedShortensTheWaitButNotTheTransfer) {
    ready();
    findSector(0);
    uint64_t t0 = cy;
    findSector(16);
    uint64_t realWait = cy - t0;

    d.setSpeed(Disk88::kRealSectorsPerSec * 50);
    d.tick(cy);
    findSector(0);
    t0 = cy;
    findSector(16);
    uint64_t fastWait = cy - t0;
    EXPECT_LT(fastWait * 10, realWait);

    std::vector<uint8_t> got;
    for (int i = 0; i < Disk88::kSectorLen; ++i) got.push_back(readByte());
    EXPECT_EQ(got, std::vector<uint8_t>(img.begin() + offsetOf(0, 16),
                                        img.begin() + offsetOf(0, 17)));
}

// CPU RESET zeroes its cycle counter; the disk must not see that as a time jump
TEST_F(TimedDisk, CycleCounterGoingBackwardDoesNotJump) {
    ready();
    uint64_t t0 = findSector(20);
    (void)t0;
    cy = 0;
    uint8_t v = in(CTL);
    EXPECT_EQ((v >> 1) & 0x1F, 20);
}

}  // namespace
