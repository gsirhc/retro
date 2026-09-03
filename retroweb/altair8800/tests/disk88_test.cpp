// GoogleTest suite for the MITS 88-DCDD controller (disk88.{h,cpp}).
//
// The controller is exercised the way the MITS boot PROM and CP/M BIOS drive
// it: OUT 0x08 to select, OUT 0x09 to step / load the head, IN 0x09 to walk the
// sector counter, IN 0x0A to stream 137-byte sectors, and the write sequence
// (OUT 0x09 bit 7, then 137x OUT 0x0A).
//
// Status bits read back INVERTED (0 = true), so the helpers below unwrap that.

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

// A synthetic image whose every byte encodes its own (track, sector, offset)
// so a misread is obvious.
std::vector<uint8_t> makeImage() {
    std::vector<uint8_t> img(Disk88::kImageSize);
    for (int t = 0; t < Disk88::kTracks; ++t)
        for (int s = 0; s < Disk88::kSectors; ++s)
            for (int b = 0; b < Disk88::kSectorLen; ++b)
                img[(t * Disk88::kSectors + s) * Disk88::kSectorLen + b] =
                    static_cast<uint8_t>((t * 7 + s * 3 + b) & 0xFF);
    return img;
}

// Read one physical 137-byte sector from the controller: sync to `sector` via
// IN 0x09, then pull 137 bytes from the data port.
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

TEST_F(DiskTest, DeselectedStatusReadsAllFalse) {
    EXPECT_EQ(d.in(SEL), 0xFF);            // inverted: 0xFF == nothing true
    d.out(SEL, 0x00);
    EXPECT_NE(d.in(SEL), 0xFF);            // a selected drive reports something
    d.out(SEL, 0x80);                      // bit 7 = deselect
    EXPECT_EQ(d.in(SEL), 0xFF);
}

TEST_F(DiskTest, HeadStepsClampAtBothEnds) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    for (int i = 0; i < 200; ++i) d.out(CTL, FN_STEP_IN);
    EXPECT_EQ(d.track(0), 76);
    for (int i = 0; i < 200; ++i) d.out(CTL, FN_STEP_OUT);
    EXPECT_EQ(d.track(0), 0);
}

// The regression that broke Burcon CP/M: the track-0 line must follow the head,
// not latch once set.
TEST_F(DiskTest, Track0LineFollowsHead) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    auto atTrack0 = [&] { return (~d.in(SEL) & 0x40) != 0; };  // bit 6, un-inverted
    EXPECT_TRUE(atTrack0());
    d.out(CTL, FN_STEP_IN);
    EXPECT_EQ(d.track(0), 1);
    EXPECT_FALSE(atTrack0());              // <- SIMH leaves this true; we don't
    d.out(CTL, FN_STEP_OUT);
    EXPECT_TRUE(atTrack0());
}

TEST_F(DiskTest, SectorCounterCyclesModulo32) {
    d.mount(0, img.data(), img.size());
    selectAndLoad();
    int first = (d.in(CTL) >> 1) & 0x1F;
    int seen = 0;
    for (int i = 0; i < 32; ++i) {
        int s = (d.in(CTL) >> 1) & 0x1F;
        EXPECT_GE(s, 0);
        EXPECT_LT(s, 32);
        seen |= 1 << s;
    }
    (void)first;
    EXPECT_EQ(seen, -1 & 0xFFFFFFFF);      // all 32 sectors observed
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
            const std::size_t base =
                (std::size_t(track) * Disk88::kSectors + sector) * Disk88::kSectorLen;
            for (int b = 0; b < Disk88::kSectorLen; ++b)
                ASSERT_EQ(got[b], img[base + b])
                    << "track " << track << " sector " << sector << " byte " << b;
        }
    }
}

TEST_F(DiskTest, WriteSequenceRoundTrips) {
    d.mount(0, img.data(), img.size());
    d.out(SEL, 0x00);
    d.out(CTL, FN_HEAD_LOAD);
    for (int i = 0; i < 3; ++i) d.out(CTL, FN_STEP_IN);   // track 3

    // sync to sector 4, arm write, push 137 bytes
    for (int g = 0; g < 64; ++g)
        if (((d.in(CTL) >> 1) & 0x1F) == 4) break;
    d.out(CTL, FN_WRITE);
    std::vector<uint8_t> payload(Disk88::kSectorLen);
    std::iota(payload.begin(), payload.end(), 0x11);
    for (uint8_t v : payload) d.out(DATA, v);
    d.out(DATA, 0x00);   // the BIOS trails the sector with fill bytes; the last
                         // one past 137 is what commits the buffer to the image

    EXPECT_TRUE(d.dirty(0));
    const std::size_t base = (std::size_t(3) * Disk88::kSectors + 4) * Disk88::kSectorLen;
    for (int b = 0; b < Disk88::kSectorLen; ++b)
        EXPECT_EQ(d.image(0)[base + b], payload[b]) << "byte " << b;

    // and it reads back
    d.out(CTL, FN_STEP_OUT); d.out(CTL, FN_STEP_IN);      // reseek track 3
    auto got = readSector(d, 4);
    EXPECT_EQ(got, payload);

    d.clearDirty(0);
    EXPECT_FALSE(d.dirty(0));
}

TEST_F(DiskTest, ReadWithNoDiskIsHarmless) {
    d.out(SEL, 0x00);
    d.out(CTL, FN_HEAD_LOAD);
    for (int i = 0; i < 10; ++i) (void)d.in(DATA);        // must not crash / OOB
    SUCCEED();
}

}  // namespace
