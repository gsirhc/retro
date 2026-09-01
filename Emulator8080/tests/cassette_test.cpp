// GoogleTest suite for the MITS 88-ACR cassette interface (cassette.{h,cpp}).
//
// The board is polled the way Altair BASIC's CLOAD/CSAVE do it:
//   read  a byte:  spin on IN 06 bit 0, then IN 07
//   write a byte:  spin on IN 06 bit 7, then OUT 07

#include <gtest/gtest.h>

#include "cassette.h"

#include <cstdint>
#include <numeric>
#include <vector>

using altair::CassetteACR;

namespace {

constexpr uint8_t STAT = 0x06;
constexpr uint8_t DATA = 0x07;
constexpr uint8_t ST_RDA = 0x01;   // 0 => a byte is available to read
constexpr uint8_t ST_TBE = 0x80;   // 0 => ready to record another byte

// read one byte the BASIC way
uint8_t tapeRead(CassetteACR &c) {
    for (int g = 0; g < 100000; ++g)
        if ((c.in(STAT) & ST_RDA) == 0) break;
    return c.in(DATA);
}
void tapeWrite(CassetteACR &c, uint8_t v) {
    for (int g = 0; g < 100; ++g)
        if ((c.in(STAT) & ST_TBE) == 0) break;
    c.out(DATA, v);
}

TEST(Cassette, TransmitAlwaysReady) {
    CassetteACR c;
    EXPECT_EQ(c.in(STAT) & ST_TBE, 0);          // recorder never blocks
}

TEST(Cassette, NoTapeNoReadData) {
    CassetteACR c;
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);          // nothing to read
}

TEST(Cassette, MountedTapePlaysBackByteForByte) {
    CassetteACR c;
    std::vector<uint8_t> tape(64);
    std::iota(tape.begin(), tape.end(), 1);
    c.mount(tape.data(), tape.size());

    std::vector<uint8_t> got;
    for (std::size_t i = 0; i < tape.size(); ++i) got.push_back(tapeRead(c));
    EXPECT_EQ(got, tape);
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);          // ran off the end
}

TEST(Cassette, RecordThenRewindThenPlay) {
    CassetteACR c;
    std::vector<uint8_t> prog(40);
    std::iota(prog.begin(), prog.end(), 0x30);

    for (uint8_t v : prog) tapeWrite(c, v);
    EXPECT_EQ(c.mode(), CassetteACR::kRecording);
    EXPECT_TRUE(c.dirty());
    EXPECT_EQ(c.data(), prog);

    c.rewind();
    std::vector<uint8_t> got;
    for (std::size_t i = 0; i < prog.size(); ++i) got.push_back(tapeRead(c));
    EXPECT_EQ(got, prog);
}

// CLOAD right after CSAVE, without touching the transport: the status poll
// auto-rewinds once the recording has gone quiet.
TEST(Cassette, AutoRewindOnPlaybackAfterRecording) {
    CassetteACR c;
    std::vector<uint8_t> prog = {1, 2, 3, 4, 5};
    for (uint8_t v : prog) tapeWrite(c, v);
    EXPECT_EQ(c.mode(), CassetteACR::kRecording);

    // BASIC now polls for read data; after a few polls the deck rewinds
    std::vector<uint8_t> got;
    for (std::size_t i = 0; i < prog.size(); ++i) got.push_back(tapeRead(c));
    EXPECT_EQ(got, prog);
}

TEST(Cassette, EjectClears) {
    CassetteACR c;
    uint8_t d[] = {9, 9, 9};
    c.mount(d, 3);
    EXPECT_TRUE(c.loaded());
    c.eject();
    EXPECT_FALSE(c.loaded());
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);
}

TEST(Cassette, RecordingOverwritesFromTheStart) {
    CassetteACR c;
    uint8_t old[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    c.mount(old, 5);
    tapeWrite(c, 0x11);
    tapeWrite(c, 0x22);
    EXPECT_EQ(c.data(), (std::vector<uint8_t>{0x11, 0x22}));
}

}  // namespace
