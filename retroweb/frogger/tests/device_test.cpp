#include <gtest/gtest.h>

#include "ay8910.h"
#include "i8255.h"
#include "video.h"

#include <array>
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
        if (v.vblank_edge) edges++;
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
    v.render(out.data());
    // native (200, 16): sy = 16+1 = 17 → row 2, ty 1 — no pixel on row 1
    EXPECT_EQ(out[200 * frogger::kUprightW + 223] & 0x00FFFFFF, 0u);
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;  // tile 2 row 1, native-left bit
    v.render(out.data());
    EXPECT_EQ(out[200 * frogger::kUprightW + 223] & 0xFF0000, 0xFF0000u);
}

TEST(Video, RiverSplitIsBlueOnNativeLeft) {
    frogger::Video v;
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> out{};
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
    v.render(out.data());
    // native (0, 16) → upright x = 223, y = 0
    EXPECT_EQ(out[0 * frogger::kUprightW + 223] & 0xFF0000, 0xFF0000u);
}
