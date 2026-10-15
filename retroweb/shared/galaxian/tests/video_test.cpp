#include <gtest/gtest.h>

#include "galaxian/video.h"

#include <array>
#include <cstdint>

TEST(Video, FrameIs50688CpuCycles) {
    EXPECT_EQ(galaxian::kCpuPerFrame, 50688);
    galaxian::Video v;
    v.reset();
    int edges = 0;
    for (int i = 0; i < galaxian::kCpuPerFrame; i++) {
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
    EXPECT_EQ(galaxian::Video::remap_color(0), 0);
    EXPECT_EQ(galaxian::Video::remap_color(2), 1);
    EXPECT_EQ(galaxian::Video::remap_color(4), 2);
}

TEST(Video, TilePixelUsesBit7AsNativeLeftAndTwoPlanes) {
    galaxian::Video v;
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
    galaxian::Video v;
    v.color_prom[0] = 0xC0;  // blue bits 6–7 set, would-be bit 0 unused
    uint32_t rgb = v.prom_rgb(0);
    EXPECT_EQ((rgb >> 16) & 0xFF, 0);
    EXPECT_EQ((rgb >> 8) & 0xFF, 0);
    EXPECT_GT(rgb & 0xFF, 0);
}

TEST(Video, ScrollNibblesSwapEnteringTheAdder) {
    galaxian::Video v;
    constexpr int col = 25;  // native x=200, road side (not the river)
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x10;  // nibble-swap → scroll 1
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8] = 0x80;  // 606 = pen 1, bit 7 = native left
    v.color_prom[1] = 0x07;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    // native (200, 16): sy = 16+1 = 17 → row 2, ty 1 — no pixel on row 1
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0x00FFFFFF, 0u);
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;  // tile 2 row 1, native-left bit
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, RiverSplitIsBlueOnNativeLeft) {
    galaxian::Video v;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    // native x=0 → upright y=0 (top); native x=200 → upright y=200 (bottom)
    EXPECT_EQ(out[0 * galaxian::kUprightW + 0] & 0x00FFFFFF, galaxian::kRiverBlue);
    EXPECT_EQ(out[200 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, Rot90MatchesMameFlipXSwapXy) {
    galaxian::Video v;
    v.videoram[2 * 32 + 0] = 2;  // row 2 col 0 = top-left of visible native
    v.objram[1] = 0;             // color attr 0 → group 0
    v.gfx[0x800 + 2 * 8] = 0x80;  // 606 = pen 1, tile 2 row 0, bit 7 = native left
    v.color_prom[1] = 0x07;       // red
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    // native (0, 16) → upright x = 223, y = 0
    EXPECT_EQ(out[0 * galaxian::kUprightW + 223] & 0xFF0000, 0xE00000u);
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

TEST(Video, ScrambleBackdropIsBlueWhenEnabled) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    v.background_enable = true;
    v.set_stars_enable(false);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[0] & 0x00FFFFFF, galaxian::kScrambleBgBlue);
}

TEST(Video, ScrambleHasNoRiverSplit) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[0 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
    EXPECT_EQ(out[200 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, ScrambleScrollDoesNotNibbleSwap) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    constexpr int col = 25;
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x01;  // no nibble-swap: scroll 1
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;
    v.color_prom[1] = 0x07;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, StarsEnableLightsPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    v.set_stars_enable(true);
    v.stars_blink_state = 3;  // mask 0xFF, no 2V skip
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out) {
        if (p & 0x00FFFFFF) lit++;
    }
    EXPECT_GT(lit, 8);
}

TEST(Video, ScrambleShellsAreTwoYellowPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int yellow = 0;
    for (uint32_t p : out) {
        if ((p & 0x00FFFFFF) == 0xFFFF00) yellow++;
    }
    EXPECT_EQ(yellow, 2);
}

TEST(Video, GalaxianHasNoRiverOrScrambleBackdrop) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.background_enable = true;
    v.set_stars_enable(false);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[0] & 0x00FFFFFF, 0u);
    EXPECT_EQ(out[200 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, GalaxianScrollDoesNotNibbleSwap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    constexpr int col = 25;
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x01;
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;
    v.color_prom[1] = 0x07;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, GalaxianStarsEnableLightsPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out) {
        if (p & 0x00FFFFFF) lit++;
    }
    EXPECT_GT(lit, 8);
}

TEST(Video, GalaxianBlueUses470And220Ohm) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.color_prom[0] = 0xC0;  // PROM bits 6 and 7
    uint32_t rgb = v.prom_rgb(0);
    EXPECT_EQ((rgb >> 16) & 0xFF, 0);
    EXPECT_EQ((rgb >> 8) & 0xFF, 0);
    EXPECT_EQ(rgb & 0xFF, 217);
}

TEST(Video, GalaxianShellsAreFourWhitePixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
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

using Frame = std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH>;

uint32_t native_at(const Frame& out, int x, int line) {
    return out[unsigned(x * galaxian::kUprightW + (galaxian::kVisH - 1) - (line - galaxian::kVisY0))] & 0x00FFFFFF;
}

void solid_sprite(galaxian::Video& v, int code) {
    for (int i = 0; i < 32; i++) {
        v.gfx[unsigned(code * 32 + i)] = 0xFF;
        v.gfx[unsigned(0x800 + code * 32 + i)] = 0xFF;
    }
}

void place_sprite(galaxian::Video& v, int n, uint8_t y, uint8_t x, uint8_t color) {
    v.objram[unsigned(0x40 + n * 4 + 0)] = y;
    v.objram[unsigned(0x40 + n * 4 + 1)] = 1;
    v.objram[unsigned(0x40 + n * 4 + 2)] = color;
    v.objram[unsigned(0x40 + n * 4 + 3)] = x;
}

}  // namespace

TEST(Video, LowerNumberedSpriteWins) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    v.color_prom[11] = 0x38;
    place_sprite(v, 0, 140, 100, 1);
    place_sprite(v, 1, 140, 100, 2);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 110, 105), v.prom_rgb(7));
}

TEST(Video, FirstThreeSpritesMatchAgainstLineMinusOne) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    place_sprite(v, 0, 140, 60, 1);
    place_sprite(v, 3, 140, 160, 1);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 70, 100), 0u);
    EXPECT_EQ(native_at(out, 70, 116), v.prom_rgb(7));
    EXPECT_EQ(native_at(out, 170, 100), v.prom_rgb(7));
    EXPECT_EQ(native_at(out, 170, 116), 0u);
}

TEST(Video, SpriteLineBufferClipsFirstSixteenPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    place_sprite(v, 3, 140, 8, 1);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == v.prom_rgb(7)) lit++;
    EXPECT_EQ(lit, 8 * 16);
    EXPECT_EQ(native_at(out, 16, 105), 0u);
    EXPECT_EQ(native_at(out, 17, 105), v.prom_rgb(7));
}

TEST(Video, GalaxianStarFieldDriftsOneStepPerFrame) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    ASSERT_EQ(v.star_origin, 0u);
    v.advance(galaxian::kCpuPerFrame);
    EXPECT_EQ(v.star_origin, uint32_t(galaxian::kStarPeriod - 1));
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame);
    v.advance(galaxian::kCpuPerFrame);
    EXPECT_EQ(v.star_origin, 1u);
}

TEST(Video, StarsEnableRestartsTheFieldAtTheBeam) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    v.advance(galaxian::kCpuPerFrame * 5);
    v.set_stars_enable(false);
    v.set_stars_enable(true);
    EXPECT_EQ(v.star_origin, 0u);
}

TEST(Video, OneShellPerLineLastMatchWins) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    v.objram[0x60 + 4 * 4 + 1] = 0xDF;
    v.objram[0x60 + 4 * 4 + 3] = 50;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int white = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == 0xFFFFFF) white++;
    EXPECT_EQ(white, 4);
    for (int x = 201; x < 205; x++) EXPECT_EQ(native_at(out, x, 32), 0xFFFFFFu);
}

TEST(Video, GalaxianMissileIsFourYellowPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 7 * 4 + 1] = 0xDF;
    v.objram[0x60 + 7 * 4 + 3] = 100;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int yellow = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == 0xFFFF00) yellow++;
    EXPECT_EQ(yellow, 4);
}

TEST(Video, ShellsOrIntoTheTileColour) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 19] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.objram[19 * 2 + 1] = 0;
    v.color_prom[1] = 0x38;
    v.objram[0x60 + 7 * 4 + 1] = 0xFF - 35;
    v.objram[0x60 + 7 * 4 + 3] = 255 - 156;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 153, 35), 0xFFFF00u);
    EXPECT_EQ(native_at(out, 158, 34), v.prom_rgb(1));
}

TEST(Video, FlipXMirrorsTheTilemap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 0] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), v.prom_rgb(1));
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), 0u);
    EXPECT_EQ(native_at(out, 252, 35), v.prom_rgb(1));
}

TEST(Video, FlipYMirrorsTheTilemap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 0] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    v.flip_y = true;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), 0u);
    EXPECT_EQ(native_at(out, 3, 220), v.prom_rgb(1));
}

TEST(Video, MidFrameWritesTakeEffectAtTheBeam) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    for (int row = 0; row < 32; row++) v.videoram[unsigned(row * 32)] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    v.advance(128 * galaxian::kCpuPerLine);
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame - 128 * galaxian::kCpuPerLine);
    Frame out{};
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 40), v.prom_rgb(1));
    EXPECT_EQ(native_at(out, 252, 40), 0u);
    EXPECT_EQ(native_at(out, 3, 200), 0u);
    EXPECT_EQ(native_at(out, 252, 200), v.prom_rgb(1));
}

TEST(Video, VblankEdgeFiresOncePerFrameAtLine240) {
    galaxian::Video v;
    v.advance(galaxian::kVBlankLine * galaxian::kCpuPerLine - 1);
    EXPECT_FALSE(v.vblank_edge);
    v.advance(1);
    EXPECT_TRUE(v.vblank_edge);
    EXPECT_TRUE(v.vblank);
    v.vblank_edge = false;
    v.advance(galaxian::kCpuPerFrame - galaxian::kVBlankLine * galaxian::kCpuPerLine);
    EXPECT_FALSE(v.vblank_edge);
    EXPECT_FALSE(v.vblank);
}
