#include <gtest/gtest.h>

#include "machine.h"
#include "video.h"
#include "wsg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

TEST(Video, FrameIs50688CpuCycles) {
    EXPECT_EQ(pacman::kCpuPerFrame, 50688);
}

TEST(Video, VblankEdgeOncePerFrame) {
    // The raster wraps to v==0 after a full frame, so check for exactly one
    // IRQ edge and that vblank goes true somewhere in the frame.
    pacman::Video v;
    v.reset();
    int edges = 0;
    bool saw_vblank = false;
    for (int i = 0; i < pacman::kCpuPerFrame; i++) {
        v.advance(1);
        if (v.vblank_edge) {
            edges++;
            v.vblank_edge = false;
        }
        if (v.vblank) saw_vblank = true;
    }
    EXPECT_EQ(edges, 1);
    EXPECT_TRUE(saw_vblank);
}

// Real pacman.5e packs 4 pixels/byte: plane 0 in the high nibble, plane 1 in
// the low nibble. Plane bytes are crafted so a row-major decode disagrees.
TEST(Video, TilePixelUsesNibblePackedRom) {
    pacman::Video v;
    v.reset();
    v.tile_rom[8] = 0x88;  // byte for x<4,y=0: bit7=1 (plane0), bit3=1 (plane1)
    EXPECT_EQ(v.tile_pixel(0, 0, 0), 3) << "pixel (0,0) should combine both nibbles of byte 8, not byte 0";
}

TEST(Video, SpritePixelUsesNibblePackedRom) {
    pacman::Video v;
    v.reset();
    v.sprite_rom[8] = 0x88;
    EXPECT_EQ(v.sprite_pixel(0, 0, 0), 3) << "sprite pixel (0,0) should read the column-slice byte at offset 8";
    v.sprite_rom[32] = 0x88;
    EXPECT_EQ(v.sprite_pixel(0, 12, 8), 3) << "sprite pixel (12,8) should land in the y>=8 half at column-byte 0+32";
}

TEST(Video, LookupRgbUsesPromDac) {
    pacman::Video v;
    v.color_prom[1] = 0x07;  // full red
    v.lookup_prom[4] = 1;    // attr 1, pix 0
    uint32_t rgb = v.lookup_rgb(1, 0);
    EXPECT_EQ((rgb >> 16) & 0xFF, 0xFF);
    EXPECT_EQ(rgb & 0xFF, 0);
}

// ROT90 = FLIP_X | SWAP_XY (MAME gamedrv.h): native (16,0) lands at upright (223,16).
TEST(Video, Rot90MatchesMameFlipXSwapXy) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;   // bright, distinguishable from black
    v.lookup_prom[1] = 31;     // attr 0, pix 1 -> color_prom[31]
    v.tile_rom[8] = 0x08;      // tile code 0, pixel (tx=0,ty=0) pen-bit-0 lit (nibble-packed ROM)
    v.videoram[64] = 0;
    v.colorram[64] = 0;

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());

    EXPECT_NE(out[16 * pacman::kUprightW + 223], 0u) << "expected lit pixel at (223,16)";
    EXPECT_EQ(out[271 * pacman::kUprightW + 0], 0u) << "mirrored-the-other-way location must stay dark";
}

// MAME draw_sprites feeds ram2[i] into sy (ram2[i] - 31) and ram2[i+1] into
// sx (272 - ram2[i+1]).
TEST(Video, SpritePositionMatchesMameRegisterRoles) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;      // attr 0, pix 1 -> color_prom[31]
    v.sprite_rom[8] = 0x08;     // sprite code 0, pixel (px=0,py=0) lit
    v.spriteram[0] = 0;         // code 0, no flip
    v.spriteram[1] = 0;         // color attr 0
    v.sprite_xy[0] = 99;        // ram2[i]     -> sy = 99 - 31 + 1 (slot 0) = 69
    v.sprite_xy[1] = 50;        // ram2[i + 1] -> sx = 272 - 50 = 222

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());

    // native (222, 69) rotates to upright dx=154, dy=222.
    EXPECT_NE(out[222 * pacman::kUprightW + 154], 0u) << "expected sprite pixel at native (222,69)";
    // The swapped-register mapping would land at dx=1, dy=69, which must stay dark.
    EXPECT_EQ(out[69 * pacman::kUprightW + 1], 0u) << "swapped-register location must stay dark";
}

TEST(Video, FlipScreenFlipsOnlyTheTilemap) {
    pacman::Video v;
    v.reset();
    v.flip_screen = true;
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;
    v.sprite_rom[8] = 0x08;
    v.sprite_xy[0] = 99;
    v.sprite_xy[1] = 50;
    v.tile_rom[16 + 8] = 0x08;   // tile 1, pixel (0,0)
    v.videoram[unsigned(2 + (0 + 2) * 32)] = 1;   // native column 4 row 0 via vram_offset
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    EXPECT_NE(out[222 * pacman::kUprightW + 154], 0u) << "sprites keep their unflipped position";
    // Tile pixel native (32,0) flips to (255,223), upright dx=0, dy=255.
    EXPECT_NE(out[255 * pacman::kUprightW + 0], 0u) << "tilemap flips X and Y";
    EXPECT_EQ(out[32 * pacman::kUprightW + 223], 0u);
}

TEST(Video, SpritesClipToTheMazeColumns) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    for (int p = 1; p < 4; p++) v.lookup_prom[unsigned(p)] = 31;
    v.sprite_rom.fill(0xFF);
    v.sprite_xy[6] = 100;
    v.sprite_xy[7] = 6;            // sx = 266, spans 266..281
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    int ny = 100 - 31 + 4;
    EXPECT_NE(out[271 * pacman::kUprightW + (pacman::kVisH - 1 - ny)], 0u);
    EXPECT_EQ(out[272 * pacman::kUprightW + (pacman::kVisH - 1 - ny)], 0u);
}

TEST(Video, SpritesWrapAt256) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    for (int p = 1; p < 4; p++) v.lookup_prom[unsigned(p)] = 31;
    v.sprite_rom.fill(0xFF);
    v.sprite_xy[6] = 100;
    v.sprite_xy[7] = 0;            // sx = 272: off the right edge, wraps to 16..31
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    int ny = 100 - 31 + 4;
    EXPECT_NE(out[20 * pacman::kUprightW + (pacman::kVisH - 1 - ny)], 0u);
}

TEST(Video, SlotsZeroToTwoSitOneLineLater) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    for (int p = 1; p < 4; p++) v.lookup_prom[unsigned(p)] = 31;
    v.sprite_rom.fill(0xFF);
    v.sprite_xy[2 * 2] = 100;       // slot 2
    v.sprite_xy[2 * 2 + 1] = 200;   // sx = 72
    v.sprite_xy[3 * 2] = 100;       // slot 3
    v.sprite_xy[3 * 2 + 1] = 100;   // sx = 172
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    auto at = [&](int nx, int ny) { return out[unsigned(nx * pacman::kUprightW + (pacman::kVisH - 1 - ny))]; };
    EXPECT_EQ(at(75, 69), 0u);
    EXPECT_NE(at(75, 70), 0u);
    EXPECT_NE(at(75, 85), 0u);
    EXPECT_NE(at(175, 69), 0u);
    EXPECT_EQ(at(175, 85), 0u);
}

TEST(Video, LowerSlotWinsTheLineBuffer) {
    pacman::Video v;
    v.reset();
    v.color_prom[1] = 0x07;
    v.color_prom[2] = 0x38;
    for (int p = 1; p < 4; p++) {
        v.lookup_prom[unsigned((1 << 2) | p)] = 1;
        v.lookup_prom[unsigned((2 << 2) | p)] = 2;
    }
    v.sprite_rom.fill(0xFF);
    v.spriteram[3 * 2 + 1] = 1;
    v.spriteram[4 * 2 + 1] = 2;
    v.sprite_xy[3 * 2] = 100;
    v.sprite_xy[3 * 2 + 1] = 100;
    v.sprite_xy[4 * 2] = 100;
    v.sprite_xy[4 * 2 + 1] = 100;
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[unsigned(175 * pacman::kUprightW + (pacman::kVisH - 1 - 75))], v.lookup_rgb(1, 1));
}

TEST(Video, MidFrameFlipTakesEffectAtTheBeam) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    for (int p = 1; p < 4; p++) v.lookup_prom[unsigned(p)] = 31;
    for (int r = 0; r < 8; r++) v.tile_rom[unsigned(16 + 8 + r)] = 0x88;
    v.videoram.fill(1);
    v.advance(2 * pacman::kCpuPerFrame);
    v.advance(112 * pacman::kCpuPerLine);
    v.flip_screen = true;
    v.tile_rom.fill(0);
    v.advance(pacman::kCpuPerFrame - 112 * pacman::kCpuPerLine);
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.render(out.data());
    EXPECT_NE(out[unsigned(32 * pacman::kUprightW + (pacman::kVisH - 1 - 10))], 0u);
    EXPECT_EQ(out[unsigned(32 * pacman::kUprightW + (pacman::kVisH - 1 - 200))], 0u);
}

// Bit 0 of the sprite's first RAM byte is X flip, bit 1 is Y flip (MAME pacman_v.cpp).
TEST(Video, SpriteFlipBitsAreXBit0YBit1) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;
    v.sprite_rom[8] = 0x08;  // sprite code 0, local pixel (px=0,py=0) lit
    v.sprite_xy[0] = 30;     // sy = 30 - 31 + 1 (slot 0) = 0
    v.sprite_xy[1] = 255;    // sx = 272 - 255 = 17 (sprite_xy is uint8_t; 0 isn't reachable)
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};

    v.spriteram[0] = 0x01;   // bit 0 set (X flip), bit 1 clear
    v.spriteram[1] = 0;
    out.fill(0);
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    // Both rotate to upright dx=(kVisH-1), dy=native_x.
    EXPECT_EQ(out[17 * pacman::kUprightW + (pacman::kVisH - 1)], 0u)
        << "bit 0 set should X-flip the sprite off native x=17";
    EXPECT_NE(out[32 * pacman::kUprightW + (pacman::kVisH - 1)], 0u)
        << "bit 0 set should X-flip native x=17 to x=32";

    v.spriteram[0] = 0x02;   // bit 1 set (Y flip only) -- X must stay put
    out.fill(0);
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());
    // Y-flip moves the pixel to native y=15 (dx=208), dy stays 17.
    EXPECT_NE(out[17 * pacman::kUprightW + (pacman::kVisH - 1 - 15)], 0u)
        << "bit 1 alone must not X-flip the sprite";
}

// MAME transpen_mask: any pen whose lookup entry is colour 0 is transparent. The ROM
// hides Pac-Man during the ghost-eaten pause with an all-black color, and an
// opaque silhouette would blot out the score sprite beneath.
TEST(Video, SpriteWithAllPensMatchingPenZeroDoesNotOccludeSpriteBehindIt) {
    pacman::Video v;
    v.reset();
    // Slot 7 (loses the line buffer): a visible sprite, color 5 pen 1 is white.
    v.color_prom[31] = 0xFF;
    v.lookup_prom[(5 << 2) | 1] = 31;
    v.sprite_rom[8] = 0x08;  // code 0, local pixel (px=0,py=0) lit (pen 1)
    v.sprite_xy[7 * 2] = 31;        // sy = 31 - 31 = 0
    v.sprite_xy[7 * 2 + 1] = 255;   // sx = 272 - 255 = 17
    v.spriteram[7 * 2] = 0;         // code 0, no flip
    v.spriteram[7 * 2 + 1] = 5;     // color 5

    // Slot 0 (wins the line buffer): same position, color 6 left black.
    v.sprite_xy[0] = 30;
    v.sprite_xy[1] = 255;
    v.spriteram[0] = 0;             // code 0, no flip
    v.spriteram[1] = 6;             // color 6 -- pens 0 and 1 both resolve to black

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());

    // native (17, 0) rotates to upright dx=(kVisH-1), dy=17.
    EXPECT_EQ(out[17 * pacman::kUprightW + (pacman::kVisH - 1)], 0xFFFFFFu)
        << "the visible sprite underneath must still show through the "
           "'invisible' (all-pens-black) sprite drawn on top of it";
}

// Ghosts leave both flip bits 0, so a pixel at local (0,15) must stay at the
// bottom (dx = 0) when unflipped.
TEST(Video, UnflippedSpriteKeepsBottomEdgeAtBottom) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;
    v.sprite_rom[8 + 7] = 0x08;  // sprite code 0, local pixel (px=0,py=7) lit (kColByte[0]=8, base_y=7)
    v.sprite_xy[0] = 30 + 7;     // sy = 7 with slot 0's extra line, so this pixel lands at native y=14
    v.sprite_xy[1] = 255;        // sx = 272 - 255 = 17
    v.spriteram[0] = 0;          // no flip -- ghosts always leave this 0
    v.spriteram[1] = 0;
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.advance(2 * pacman::kCpuPerFrame);
    v.render(out.data());

    // native (17, 14) rotates to upright dx=(kVisH-1)-14=209, dy=17.
    EXPECT_NE(out[17 * pacman::kUprightW + 209], 0u)
        << "unflipped sprite pixel should land at its native position, not be mirrored";
}

namespace {

float wsg_peak(pacman::Wsg& w) {
    std::vector<float> out;
    w.advance(3072, 48000, out);
    float p = 0;
    for (float s : out) p = std::max(p, std::fabs(s));
    return p;
}

}  // namespace

TEST(Wsg, SilentWhenDisabled) {
    pacman::Wsg w;
    w.reset();
    w.regs[0x11] = 1;
    w.regs[0x15] = 0x0F;
    w.wave_prom.fill(0x0F);
    EXPECT_EQ(wsg_peak(w), 0);
    w.enabled = true;
    EXPECT_GT(wsg_peak(w), 0);
}

// Real register map (MAME namco.cpp pacman_sound_w): only 0x05/0x0a/0x0f
// (waveform), 0x10-0x15 (ch0), 0x16-0x1a (ch1), 0x1b-0x1f (ch2) are wired.
TEST(Wsg, RegisterMapMatchesRealHardwareOffsets) {
    pacman::Wsg w;
    w.reset();
    w.enabled = true;
    w.wave_prom.fill(0x0F);
    w.write(0x1b, 0x01);
    w.write(0x1f, 0x0F);
    w.write(0x0f, 0x00);
    EXPECT_GT(wsg_peak(w), 0) << "ch2 is audible from its own offsets";

    w.reset();
    w.enabled = true;
    w.write(0x0a, 0x01);
    w.write(0x17, 0x0F);
    w.write(0x07, 0x00);
    EXPECT_EQ(wsg_peak(w), 0) << "ch1 has no volume, and 0x07 is unwired";
}

TEST(Wsg, OnlyVoiceZeroHasLowFrequencyNibble) {
    pacman::Wsg w;
    w.reset();
    w.enabled = true;
    w.wave_prom.fill(0x0F);
    w.write(0x15, 0x0F);
    w.write(0x10, 0x01);
    EXPECT_GT(wsg_peak(w), 0);
    w.reset();
    w.enabled = true;
    w.write(0x1a, 0x0F);
    w.write(0x10, 0x01);
    EXPECT_EQ(wsg_peak(w), 0) << "ch1 ignores 0x10";
}

TEST(Wsg, AdvancingWithoutOutputStillTurnsThePhase) {
    pacman::Wsg a, b;
    for (pacman::Wsg* w : {&a, &b}) {
        w->reset();
        w->enabled = true;
        for (int i = 0; i < 256; i++) w->wave_prom[unsigned(i)] = uint8_t(i & 15);
        w->write(0x13, 0x08);  // freq = 1 << 15: one wave step per WSG clock
        w->write(0x15, 0x0F);
    }
    std::vector<float> none, sa, sb;
    a.advance(32 * 5, 0, none);
    EXPECT_TRUE(none.empty());
    a.advance(32, 96000, sa);
    b.advance(32, 96000, sb);
    ASSERT_EQ(sa.size(), 1u);
    ASSERT_EQ(sb.size(), 1u);
    EXPECT_FLOAT_EQ(sa[0], (6 - 8) * 15 / 360.0f);
    EXPECT_FLOAT_EQ(sb[0], (1 - 8) * 15 / 360.0f);
}

// Frequency writes must not jump the waveform phase.
TEST(Wsg, FrequencyWriteKeepsPhaseContinuous) {
    pacman::Wsg w;
    w.reset();
    w.enabled = true;
    for (int i = 0; i < 256; i++) w.wave_prom[unsigned(i)] = uint8_t(i & 15);
    w.write(0x05, 0);
    w.write(0x13, 1);  // freq = 0x1000
    w.write(0x15, 15);
    std::vector<float> a;
    w.advance(pacman::kCpuHz / 20, 48000, a);
    ASSERT_GT(a.size(), 10u);
    double step = 0;
    for (size_t i = 1; i < a.size(); i++) step += std::fabs(a[i] - a[i - 1]);
    step /= double(a.size() - 1);
    w.write(0x13, 2);  // freq = 0x2000
    size_t mid = a.size();
    w.advance(pacman::kCpuHz / 50, 48000, a);
    ASSERT_GT(a.size(), mid);
    float jump = std::fabs(a[mid] - a[mid - 1]);
    EXPECT_LT(jump, step * 4.0) << "freq write jumped phase (jump=" << jump << " avg step=" << step << ")";
}
