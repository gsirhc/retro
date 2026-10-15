#include <gtest/gtest.h>

#include "galaxian/ay8910.h"

#include <vector>

TEST(Ay8910, RegistersReadBackThroughTheirWidthMasks) {
    galaxian::Ay8910 ay;
    ay.reset();
    const uint8_t want[14] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F};
    for (uint8_t r = 0; r < 14; r++) {
        ay.write_addr(r);
        ay.write_data(0xFF);
        EXPECT_EQ(ay.read_data(), want[r]) << "R" << int(r);
    }
}

TEST(Ay8910, ToneAIsAudibleWhenEnabled) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.write_addr(0);
    ay.write_data(0x20);
    ay.write_addr(1);
    ay.write_data(0x00);
    ay.write_addr(7);
    ay.write_data(0x38);  // tone A on, noise off
    ay.write_addr(8);
    ay.write_data(0x0F);
    std::vector<float> audio;
    ay.advance(1789772 / 20, 48000, audio);  // 50 ms
    ASSERT_FALSE(audio.empty());
    bool any = false;
    for (float s : audio) {
        if (s != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any);
}

TEST(Ay8910, ToneFrequencyIsClockOver16Period) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.write_addr(0);
    ay.write_data(32);
    ay.write_addr(1);
    ay.write_data(0);
    ay.write_addr(7);
    ay.write_data(0x38);
    ay.write_addr(8);
    ay.write_data(0x0F);
    std::vector<float> audio;
    ay.advance(1789772 / 5, 48000, audio);
    int rises = 0;
    for (size_t i = 1; i < audio.size(); i++) {
        if (audio[i - 1] <= 0.0f && audio[i] > 0.0f) rises++;
    }
    double hz = double(rises) / (double(audio.size()) / 48000.0);
    EXPECT_NEAR(hz, 1789772.0 / (16.0 * 32.0), 200.0);
}

TEST(Ay8910, EnvelopeDecayLastsSixteenSteps) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.write_addr(7);
    ay.write_data(0x3F);  // tone and noise off: envelope still modulates
    ay.write_addr(8);
    ay.write_data(0x10);
    ay.write_addr(11);
    ay.write_data(0x00);
    ay.write_addr(12);
    ay.write_data(0x01);  // EP=256 → 16 steps × 16×256 clocks ≈ 37 ms
    ay.write_addr(13);
    ay.write_data(0x00);
    std::vector<float> early;
    ay.advance(int(1789772 * 0.010), 48000, early);
    bool mid = false;
    for (float s : early) {
        if (s > 0.001f) { mid = true; break; }
    }
    EXPECT_TRUE(mid) << "decay still running at 10 ms";
    std::vector<float> rest;
    ay.advance(int(1789772 * 0.050), 48000, rest);
    EXPECT_EQ(ay.mix(), 0.0f) << "CONT=0 holds 0 after 16 steps";
}

TEST(Ay8910, MuteSilencesMix) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.write_addr(8);
    ay.write_data(0x0F);
    ay.write_addr(7);
    ay.write_data(0x38);
    ay.mute = true;
    EXPECT_EQ(ay.mix(), 0.0f);
}

TEST(Ay8910, PortAReadUsesCallback) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.port_a_r = [] { return uint8_t(0x42); };
    ay.write_addr(14);
    EXPECT_EQ(ay.read_data(), 0x42);
}

TEST(Ay8910, OutputLevelIsZeroWhileTheMixerGatesTheChannel) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x0C;
    EXPECT_EQ(ay.output_level(0), 0x0C);
    ay.regs[7] = 0x3E;
    EXPECT_EQ(ay.output_level(0), 0);
}

TEST(Ay8910, MosfetOutputResistanceFallsWithLevel) {
    EXPECT_GT(galaxian::Ay8910::output_resistance(0), 1e8);
    EXPECT_NEAR(galaxian::Ay8910::output_resistance(15), 4268.0, 5.0);
    for (int l = 1; l < 16; l++)
        EXPECT_LT(galaxian::Ay8910::output_resistance(uint8_t(l)), galaxian::Ay8910::output_resistance(uint8_t(l - 1)));
}

namespace {

uint8_t held_envelope(uint8_t shape) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x10;
    ay.regs[11] = 1;
    ay.write_addr(13);
    ay.write_data(shape);
    for (int i = 0; i < 16 * 100; i++) ay.tick();
    return ay.output_level(0);
}

// Envelope level at each step for `n` steps, EP = 1, stepped with no host output.
std::vector<int> envelope_steps(uint8_t shape, int n) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x10;
    ay.write_addr(11);
    ay.write_data(1);
    ay.write_addr(13);
    ay.write_data(shape);
    std::vector<int> out;
    std::vector<float> none;
    for (int i = 0; i < n; i++) {
        out.push_back(ay.output_level(0));
        ay.advance(16, 0, none);
    }
    EXPECT_TRUE(none.empty());
    return out;
}

}  // namespace

TEST(Ay8910, RepeatingEnvelopeShapesMatchTheDatasheet) {
    const int n = 64;
    std::vector<int> saw_down, triangle_down, saw_up, triangle_up;
    for (int i = 0; i < n; i++) {
        int p = i % 32;
        saw_down.push_back(15 - i % 16);
        saw_up.push_back(i % 16);
        triangle_down.push_back(p < 16 ? 15 - p : p - 16);
        triangle_up.push_back(p < 16 ? p : 31 - p);
    }
    EXPECT_EQ(envelope_steps(0x08, n), saw_down);
    EXPECT_EQ(envelope_steps(0x0A, n), triangle_down);
    EXPECT_EQ(envelope_steps(0x0C, n), saw_up);
    EXPECT_EQ(envelope_steps(0x0E, n), triangle_up);
}

TEST(Ay8910, HeldEnvelopeLevelsMatchTheDatasheet) {
    EXPECT_EQ(held_envelope(0x00), 0);
    EXPECT_EQ(held_envelope(0x04), 0);
    EXPECT_EQ(held_envelope(0x09), 0);
    EXPECT_EQ(held_envelope(0x0B), 15);
    EXPECT_EQ(held_envelope(0x0D), 15);
    EXPECT_EQ(held_envelope(0x0F), 0);
}
