// GoogleTest suite for the MITS 88-ACR cassette interface (cassette.{h,cpp}).
//
// The board is polled the way Altair BASIC's CLOAD/CSAVE do it:
//   read  a byte:  spin on IN 06 bit 0, then IN 07
//   write a byte:  spin on IN 06 bit 7, then OUT 07

#include <gtest/gtest.h>

#include "cassette.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

using altair::CassetteACR;

namespace {

constexpr uint8_t STAT = 0x06;
constexpr uint8_t DATA = 0x07;
constexpr uint8_t ST_RDA = 0x01;   // 0 => a byte is available to read
constexpr uint8_t ST_TBE = 0x80;   // 0 => ready to record another byte

// read one byte the BASIC way (PLAY held)
uint8_t tapeRead(CassetteACR &c) {
    c.setMotor(true);
    for (int g = 0; g < 100000; ++g)
        if ((c.in(STAT) & ST_RDA) == 0) break;
    return c.in(DATA);
}
// write one byte the BASIC way (PLAY + REC held)
void tapeWrite(CassetteACR &c, uint8_t v) {
    c.setMotor(true);
    c.setRecordArm(true);
    for (int g = 0; g < 100; ++g)
        if ((c.in(STAT) & ST_TBE) == 0) break;
    c.out(DATA, v);
}

TEST(Cassette, RecorderReadyOnlyWhenArmed) {
    CassetteACR c;
    c.setMotor(true);
    c.setRecordArm(true);
    EXPECT_EQ(c.in(STAT) & ST_TBE, 0);          // PLAY + REC -> ready to record
    c.setRecordArm(false);
    EXPECT_NE(c.in(STAT) & ST_TBE, 0);          // REC released -> not ready
    c.out(DATA, 0x42);
    EXPECT_TRUE(c.data().empty());              // and a write is ignored
    c.setRecordArm(true);
    EXPECT_EQ(c.in(STAT) & ST_TBE, 0);
    c.out(DATA, 0x42);
    EXPECT_EQ(c.data(), (std::vector<uint8_t>{0x42}));
}

// The record interlock needs PLAY as well as REC -- no capstan, no recording.
TEST(Cassette, RecordingNeedsTheMotorToo) {
    CassetteACR c;
    c.setRecordArm(true);                       // REC pressed, but not PLAY
    EXPECT_NE(c.in(STAT) & ST_TBE, 0);          // not ready without the capstan
    c.out(DATA, 0x99);
    EXPECT_TRUE(c.data().empty());
}

TEST(Cassette, NoTapeNoReadData) {
    CassetteACR c;
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);          // nothing to read
}

TEST(Cassette, AccessorsAndUnownedPort) {
    CassetteACR c;
    EXPECT_TRUE(c.owns(0x06));
    EXPECT_FALSE(c.owns(0x05));
    EXPECT_EQ(c.in(0x05), 0xFF);                // a port the board doesn't own
    EXPECT_FALSE(c.motor());
    EXPECT_EQ(c.wind(), 0);
    EXPECT_EQ(c.len(), 0u);
    EXPECT_EQ(c.ioTicks(), 0u);

    uint8_t d[] = {1, 2, 3};
    c.mount(d, 3);
    c.setMotor(true);
    (void)tapeRead(c);
    EXPECT_EQ(c.len(), 3u);
    EXPECT_GE(c.ioTicks(), 1u);
    EXPECT_TRUE(c.motor());
    c.setWind(-1);                              // sets wind_ (throttled speed keeps it engaged)
    c.setSpeed(30);
    c.setWind(-1);
    EXPECT_EQ(c.wind(), -1);
    c.clearDirty();
    EXPECT_FALSE(c.dirty());
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

// After CSAVE the head sits past the recording (no auto-rewind) -- CLOAD needs a
// manual REW first, exactly like a real recorder.
TEST(Cassette, RecordingLeavesHeadPastTheData) {
    CassetteACR c;
    std::vector<uint8_t> prog = {1, 2, 3, 4, 5};
    for (uint8_t v : prog) tapeWrite(c, v);
    EXPECT_EQ(c.pos(), prog.size());

    c.setRecordArm(false);                       // STOP releases REC
    EXPECT_NE(c.mode(), CassetteACR::kRecording);
    EXPECT_EQ(c.pos(), prog.size());             // head did NOT rewind itself

    c.rewind();
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

// Playback only feeds while the motor is engaged (PLAY on the deck).
TEST(Cassette, MotorGatesPlayback) {
    CassetteACR c;
    std::vector<uint8_t> tape = {11, 22, 33};
    c.mount(tape.data(), tape.size());

    c.setMotor(false);
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);          // stopped: no byte available
    EXPECT_EQ(c.in(DATA), 0);                   // and a read yields nothing / no advance

    c.setMotor(true);
    std::vector<uint8_t> got;
    for (std::size_t i = 0; i < tape.size(); ++i) got.push_back(tapeRead(c));
    EXPECT_EQ(got, tape);
}

// With a byte-rate throttle set, a byte becomes readable only once tick() has
// credited enough CPU cycles for it -- so BASIC can't outrun the tape.
TEST(Cassette, SpeedThrottleGatesOnCpuCycles) {
    CassetteACR c;
    std::vector<uint8_t> tape = {10, 20, 30};
    c.mount(tape.data(), tape.size());
    c.setMotor(true);
    c.setSpeed(30);                              // 300 baud -> 66'666 cycles/byte at 2 MHz

    c.tick(0);
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);           // no credit yet -> nothing ready

    c.tick(66666);                              // one byte-time has gone by
    EXPECT_EQ(c.in(STAT) & ST_RDA, 0);
    EXPECT_EQ(c.in(DATA), 10);
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);           // credit spent

    c.tick(66666 + 30000);                       // not enough for another
    EXPECT_NE(c.in(STAT) & ST_RDA, 0);

    c.tick(66666 * 2 + 200);                     // now a second byte-time is up
    EXPECT_EQ(c.in(STAT) & ST_RDA, 0);
    EXPECT_EQ(c.in(DATA), 20);
}

// At 25x / 50x the CLOAD loop drains several byte-times per tick(), so a batch
// of bytes becomes readable in one frame -- the point of the fast modes.
TEST(Cassette, FastSpeedDeliversManyBytesPerTick) {
    CassetteACR c;
    std::vector<uint8_t> tape(400);
    std::iota(tape.begin(), tape.end(), 0);
    c.mount(tape.data(), tape.size());
    c.setMotor(true);
    c.setSpeed(1500);                            // 50x of 300 baud -> ~1333 cycles/byte

    c.tick(0);
    c.tick(33333);                               // one 60 fps frame at 2 MHz
    int got = 0;
    while ((c.in(STAT) & ST_RDA) == 0 && got < 400) { c.in(DATA); ++got; }
    EXPECT_GT(got, 15);                          // ~25 bytes -- not frame-capped at 1
}

TEST(Cassette, UnlimitedSpeedNeverGates) {
    CassetteACR c;
    std::vector<uint8_t> tape(32);
    std::iota(tape.begin(), tape.end(), 1);
    c.mount(tape.data(), tape.size());
    c.setMotor(true);
    c.setSpeed(0);                               // unlimited

    std::vector<uint8_t> got;
    for (std::size_t i = 0; i < tape.size(); ++i) {
        EXPECT_EQ(c.in(STAT) & ST_RDA, 0);       // always ready, no tick() needed
        got.push_back(c.in(DATA));
    }
    EXPECT_EQ(got, tape);
}

// Recording writes at the head and leaves the old tail on the tape.
TEST(Cassette, RecordingOverwritesInPlace) {
    CassetteACR c;
    uint8_t old[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    c.mount(old, 5);
    tapeWrite(c, 0x11);
    tapeWrite(c, 0x22);
    EXPECT_EQ(c.data(), (std::vector<uint8_t>{0x11, 0x22, 0xCC, 0xDD, 0xEE}));
    EXPECT_EQ(c.pos(), 2u);
}

// Two programs on one tape: record A, STOP, record B where the head is left --
// the tape holds A, a blank gap, then B. Nothing is wiped.
TEST(Cassette, RecordAppendsForMultipleFiles) {
    CassetteACR c;
    c.mount(nullptr, 0);                         // blank tape
    std::vector<uint8_t> a = {0xA0, 0xA1, 0xA2};
    std::vector<uint8_t> b = {0xB0, 0xB1};
    for (uint8_t v : a) tapeWrite(c, v);
    c.setRecordArm(false);                       // STOP
    c.setRecordArm(true);                        // REC again -- head still past A
    for (uint8_t v : b) tapeWrite(c, v);

    const auto &tape = c.data();
    EXPECT_GT(tape.size(), a.size() + b.size());                 // a gap sits between
    EXPECT_TRUE(std::equal(a.begin(), a.end(), tape.begin()));   // A is still there
    EXPECT_TRUE(std::equal(b.rbegin(), b.rend(), tape.rbegin()));  // B follows it
}

// FAST-FORWARD winds the head toward the tape end at kWindMult x the byte rate,
// then auto-stops when it gets there.
TEST(Cassette, WindForwardHonoursSpeedAndAutoStopsAtEnd) {
    CassetteACR c;
    std::vector<uint8_t> tape(200);
    c.mount(tape.data(), tape.size());
    c.setSpeed(30);                              // 66'666 cycles/byte
    c.tick(0);
    c.setWind(+1);
    EXPECT_EQ(c.transport(), CassetteACR::kFwd);

    std::size_t last = c.pos();
    for (uint64_t t = 100000; t <= 200000000ull && c.transport() == CassetteACR::kFwd; t += 200000)
        c.tick(t);
    EXPECT_GT(c.pos(), last);
    EXPECT_EQ(c.pos(), c.capacity());            // wound to the physical end
    EXPECT_EQ(c.transport(), CassetteACR::kStop);  // and the key popped up
}

TEST(Cassette, WindBackAutoStopsAtStart) {
    CassetteACR c;
    std::vector<uint8_t> tape(5000);
    c.mount(tape.data(), tape.size());
    c.setSpeed(30);
    c.tick(0);
    // wind forward a bit first
    c.setWind(+1);
    uint64_t t = 100000;
    for (; t <= 40000000ull && c.pos() < 2000; t += 100000) c.tick(t);
    c.setWind(0);
    EXPECT_GT(c.pos(), 0u);

    c.setWind(-1);
    for (; c.transport() == CassetteACR::kRewind && t < 400000000ull; t += 200000)
        c.tick(t);
    EXPECT_EQ(c.pos(), 0u);
    EXPECT_EQ(c.transport(), CassetteACR::kStop);
}

// PLAY with nothing reading the board: the tape still rolls past the head.
TEST(Cassette, PlayFreeRunsWhenNothingReads) {
    CassetteACR c;
    std::vector<uint8_t> tape(4000);
    c.mount(tape.data(), tape.size());
    c.setSpeed(30);
    c.setMotor(true);                            // PLAY, but no IN 0x07 ever
    c.tick(0);
    for (uint64_t t = 1000000; t <= 60000000ull; t += 1000000) c.tick(t);
    EXPECT_GT(c.pos(), 0u);                      // the head advanced on its own
}

// PLAY with nobody reading eventually runs the tape off the end -> auto-stop.
TEST(Cassette, PlayRunsOffTheEndAndStops) {
    CassetteACR c;
    std::vector<uint8_t> tape(64);
    c.mount(tape.data(), tape.size());
    c.setSpeed(150);                             // fast enough to reach the end quickly
    c.setMotor(true);
    c.tick(0);
    for (uint64_t t = 5000000; c.transport() == CassetteACR::kPlay && t < 3000000000ull; t += 5000000)
        c.tick(t);
    EXPECT_EQ(c.pos(), c.capacity());
    EXPECT_EQ(c.transport(), CassetteACR::kStop);   // motor released itself
    EXPECT_EQ(c.in(0x07), 0);                       // nothing feeds -- transport is stopped
}

// A throttled CSAVE: the head only moves on each OUT 0x07, and the credit that
// builds between writes is capped, never spilled forward.
TEST(Cassette, RecordingCreditIsCappedNotSpilled) {
    CassetteACR c;
    c.mount(nullptr, 0);
    c.setSpeed(30);
    c.setMotor(true);
    c.setRecordArm(true);
    c.tick(0);
    c.out(DATA, 0x5A);                           // first byte -> recording mode
    EXPECT_EQ(c.pos(), 1u);
    c.tick(500000000ull);                        // a long quiet spell, lots of credit
    EXPECT_EQ(c.pos(), 1u);                      // the head did NOT roll on
    EXPECT_EQ(c.in(STAT) & ST_TBE, 0);          // still ready for the next byte
    c.out(DATA, 0x3C);
    EXPECT_EQ(c.data(), (std::vector<uint8_t>{0x5A, 0x3C}));
}

// "Max" makes a wind an instant seek.
TEST(Cassette, UnlimitedSpeedWindIsInstant) {
    CassetteACR c;
    std::vector<uint8_t> tape(100);
    c.mount(tape.data(), tape.size());
    c.setSpeed(0);                               // Max

    c.setWind(+1);
    EXPECT_EQ(c.pos(), c.capacity());
    EXPECT_EQ(c.transport(), CassetteACR::kStop);
    c.setWind(-1);
    EXPECT_EQ(c.pos(), 0u);
}

}  // namespace
