#include <gtest/gtest.h>

#include "ay8910.h"
#include "galaxian/konami_sound.h"
#include "i8255.h"
#include "video.h"

#include <array>
#include <cmath>
#include <vector>

TEST(I8255, ResetLeavesPortsAsInputs) {
    frogger::I8255 p;
    p.reset();
    p.in_a = [] { return uint8_t(0xA5); };
    EXPECT_TRUE(p.a_in);
    EXPECT_EQ(p.read(0), 0xA5);
}

TEST(I8255, Mode0AllOutputsLatchesAndCallbacks) {
    frogger::I8255 p;
    p.reset();
    uint8_t seen = 0;
    p.out_a = [&](uint8_t v) { seen = v; };
    p.write(3, 0x80);  // mode 0, all outputs
    EXPECT_FALSE(p.a_in);
    p.write(0, 0x3C);
    EXPECT_EQ(p.a, 0x3C);
    EXPECT_EQ(seen, 0x3C);
    EXPECT_EQ(p.read(0), 0x3C);
}

TEST(Ay8910, ToneAIsAudibleWhenEnabled) {
    frogger::Ay8910 ay;
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
    frogger::Ay8910 ay;
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
    frogger::Ay8910 ay;
    ay.reset();
    ay.write_addr(7);
    ay.write_data(0x3F);
    ay.write_addr(8);
    ay.write_data(0x10);
    ay.write_addr(11);
    ay.write_data(0x00);
    ay.write_addr(12);
    ay.write_data(0x01);
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
    frogger::Ay8910 ay;
    ay.reset();
    ay.write_addr(8);
    ay.write_data(0x0F);
    ay.write_addr(7);
    ay.write_data(0x38);
    ay.mute = true;
    EXPECT_EQ(ay.mix(), 0.0f);
}

TEST(Ay8910, PortAReadUsesCallback) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.port_a_r = [] { return uint8_t(0x42); };
    ay.write_addr(14);
    EXPECT_EQ(ay.read_data(), 0x42);
}

TEST(Video, FrameIs50688CpuCycles) {
    EXPECT_EQ(frogger::kCpuPerFrame, 50688);
    frogger::Video v;
    v.reset();
    int edges = 0;
    for (int i = 0; i < frogger::kCpuPerFrame; i++) {
        v.advance(1);
        if (v.vblank_edge) {
            edges++;
            v.vblank_edge = false;
        }
    }
    EXPECT_EQ(edges, 1);
    EXPECT_EQ(v.v, 0);
    EXPECT_FALSE(v.vblank);
}

TEST(Video, RemapColorMatchesFroggerPromWiring) {
    EXPECT_EQ(frogger::Video::remap_color(0), 0);
    EXPECT_EQ(frogger::Video::remap_color(2), 1);
    EXPECT_EQ(frogger::Video::remap_color(4), 2);
}

TEST(Video, TilePixelUsesBit7AsNativeLeftAndTwoPlanes) {
    frogger::Video v;
    v.gfx[0] = 0x80;          // 607 high plane, tile 0 row 0, bit 7 = native left
    v.gfx[0x800] = 0x80;      // 606 low plane same bit → pix 3
    EXPECT_EQ(v.tile_pixel(0, 0, 0), 3);
    EXPECT_EQ(v.tile_pixel(0, 1, 0), 0);
    v.gfx[0] = 0x80;
    v.gfx[0x800] = 0;
    EXPECT_EQ(v.tile_pixel(0, 0, 0), 2);
    v.gfx[0] = 0;
    v.gfx[0x800] = 0x80;
    EXPECT_EQ(v.tile_pixel(0, 0, 0), 1);
}

TEST(Video, PromRgbBlueBit0Unconnected) {
    frogger::Video v;
    v.color_prom[0] = 0xC0;  // blue bits 6–7 set, would-be bit 0 unused
    uint32_t rgb = v.prom_rgb(0);
    EXPECT_EQ((rgb >> 16) & 0xFF, 0);
    EXPECT_EQ((rgb >> 8) & 0xFF, 0);
    EXPECT_GT(rgb & 0xFF, 0);
}

TEST(Video, ScrollNibblesSwapEnteringTheAdder) {
    frogger::Video v;
    constexpr int col = 25;  // native x=200, road side (not the river)
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x10;  // nibble-swap → scroll 1
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8] = 0x80;  // 606 = pen 1, bit 7 = native left
    v.color_prom[1] = 0x07;
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> out{};
    v.advance(frogger::kCpuPerFrame);
    v.render(out.data());
    // native (200, 16): sy = 16+1 = 17 → row 2, ty 1 — no pixel on row 1
    EXPECT_EQ(out[200 * frogger::kUprightW + 223] & 0x00FFFFFF, 0u);
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;  // tile 2 row 1, native-left bit
    v.advance(frogger::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[200 * frogger::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, RiverSplitIsBlueOnNativeLeft) {
    frogger::Video v;
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> out{};
    v.advance(frogger::kCpuPerFrame);
    v.render(out.data());
    // native x=0 → upright y=0 (top); native x=200 → upright y=200 (bottom)
    EXPECT_EQ(out[0 * frogger::kUprightW + 0] & 0x00FFFFFF, frogger::kRiverBlue);
    EXPECT_EQ(out[200 * frogger::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, Rot90MatchesMameFlipXSwapXy) {
    frogger::Video v;
    v.videoram[2 * 32 + 0] = 2;  // row 2 col 0 = top-left of visible native
    v.objram[1] = 0;             // color attr 0 → group 0
    v.gfx[0x800 + 2 * 8] = 0x80;  // 606 = pen 1, tile 2 row 0, bit 7 = native left
    v.color_prom[1] = 0x07;       // red
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> out{};
    v.advance(frogger::kCpuPerFrame);
    v.render(out.data());
    // native (0, 16) → upright x = 223, y = 0
    EXPECT_EQ(out[0 * frogger::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, FroggerShellsAreFourWhitePixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Frogger;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int white = 0;
    for (uint32_t p : out) {
        if ((p & 0x00FFFFFF) == 0xFFFFFF) white++;
    }
    EXPECT_EQ(white, 4);
}

namespace {

uint8_t held_envelope(uint8_t shape) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x10;
    ay.regs[11] = 1;
    ay.write_addr(13);
    ay.write_data(shape);
    for (int i = 0; i < 16 * 100; i++) ay.tick();
    return ay.output_level(0);
}

double konami_rms(bool filtered, int tone_period) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.regs[0] = uint8_t(tone_period);
    ay.regs[7] = 0x3E;
    ay.regs[8] = 0x0F;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    if (filtered) k.filter_w(3 << 6);
    std::vector<float> out;
    k.advance(1789772 / 10, 48000, out);
    double sum = 0;
    for (size_t i = out.size() / 2; i < out.size(); i++) sum += double(out[i]) * out[i];
    return std::sqrt(sum / double(out.size() - out.size() / 2));
}

}  // namespace

TEST(Ay8910, HeldEnvelopeLevelsMatchTheDatasheet) {
    EXPECT_EQ(held_envelope(0x00), 0);
    EXPECT_EQ(held_envelope(0x04), 0);
    EXPECT_EQ(held_envelope(0x09), 0);
    EXPECT_EQ(held_envelope(0x0B), 15);
    EXPECT_EQ(held_envelope(0x0D), 15);
    EXPECT_EQ(held_envelope(0x0F), 0);
}

TEST(Ay8910, OutputLevelIsZeroWhileTheMixerGatesTheChannel) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x0C;
    EXPECT_EQ(ay.output_level(0), 0x0C);
    ay.regs[7] = 0x3E;
    EXPECT_EQ(ay.output_level(0), 0);
}

TEST(Ay8910, MosfetOutputResistanceFallsWithLevel) {
    EXPECT_GT(frogger::Ay8910::output_resistance(0), 1e8);
    EXPECT_NEAR(frogger::Ay8910::output_resistance(15), 4268.0, 5.0);
    for (int l = 1; l < 16; l++)
        EXPECT_LT(frogger::Ay8910::output_resistance(uint8_t(l)), frogger::Ay8910::output_resistance(uint8_t(l - 1)));
}

TEST(KonamiSound, SwitchedCapsDampTheTone) {
    double dry = konami_rms(false, 32);
    double wet = konami_rms(true, 32);
    EXPECT_GT(dry, 0.05);
    EXPECT_LT(wet, dry * 0.5);
}

TEST(KonamiSound, OutputIsAcCoupled) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x0F;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    std::vector<float> out;
    k.advance(1789772 / 2, 48000, out);
    ASSERT_FALSE(out.empty());
    EXPECT_GT(std::fabs(out.front()), 0.1f);
    EXPECT_LT(std::fabs(out.back()), 1e-3f);
}

TEST(KonamiSound, MuteSilencesTheOutput) {
    frogger::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3E;
    ay.regs[8] = 0x0F;
    ay.regs[0] = 32;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    k.mute = true;
    std::vector<float> out;
    k.advance(1789772 / 20, 48000, out);
    for (float s : out) EXPECT_EQ(s, 0.0f);
}
