#include <gtest/gtest.h>

#include "video.h"
#include "wsg.h"

#include <array>

TEST(Video, FrameIs50688CpuCycles) {
    EXPECT_EQ(pacman::kCpuPerFrame, 50688);
}

TEST(Video, VblankEdgeOncePerFrame) {
    // A full frame's cycle count (kCpuPerFrame) ends back at v==0 -- active
    // display, not vblank -- since the raster wraps exactly there; vblank
    // only holds for lines 224-263 of the 264-line frame. So the real
    // invariant to check is "exactly one IRQ-triggering edge, and vblank
    // does go true somewhere in the frame", not "still true at the last tick".
    pacman::Video v;
    v.reset();
    int edges = 0;
    bool saw_vblank = false;
    for (int i = 0; i < pacman::kCpuPerFrame; i++) {
        v.advance(1);
        if (v.vblank_edge) edges++;
        if (v.vblank) saw_vblank = true;
    }
    EXPECT_EQ(edges, 1);
    EXPECT_TRUE(saw_vblank);
}

// Real pacman.5e packs 4 pixels/byte (low/high nibble = the two bit planes
// of a 4-pixel column slice), not 8 rows of 1bpp per plane. This is tile
// code 0 with plane bytes crafted so the naive row-major decode and the
// real nibble-packed decode disagree at (x=0,y=0): naive reads t[0] bit7
// for plane0 and t[8] bit7 for plane1; the real format reads t[8] (since
// x<4) with plane0 in the HIGH nibble (bit 7-(x&3)) and plane1 in the LOW
// nibble (bit 3-(x&3)).
TEST(Video, TilePixelUsesNibblePackedRom) {
    pacman::Video v;
    v.reset();
    v.tile_rom[8] = 0x88;  // byte for x<4,y=0: bit7=1 (plane0), bit3=1 (plane1)
    EXPECT_EQ(v.tile_pixel(0, 0, 0), 3) << "pixel (0,0) should combine both nibbles of byte 8, not byte 0";
}

TEST(Video, SpritePixelUsesNibblePackedRom) {
    pacman::Video v;
    v.reset();
    // x=0..3 -> column-slice byte at offset kColByte[0]=8; y=0 -> +0.
    v.sprite_rom[8] = 0x88;
    EXPECT_EQ(v.sprite_pixel(0, 0, 0), 3) << "sprite pixel (0,0) should read the column-slice byte at offset 8";
    // y=8..15 slice uses base_y = 32 + (y-8); x=12..15 -> kColByte[3]=0.
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

// MAME tags every pacman.cpp driver ROT90, defined as FLIP_X | SWAP_XY
// (src/emu/gamedrv.h): swap the axes, then mirror the result's X. A single
// lit tile pixel at native (col=2,row=0,tx=0,ty=0) -- i.e. native (16,0) --
// must land at upright ((kVisH-1)-0, 16) = (223, 16), not the mirrored
// (0, 287-16) a same-magnitude rotation the other way would produce.
TEST(Video, Rot90MatchesMameFlipXSwapXy) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;   // bright, distinguishable from black
    v.lookup_prom[1] = 31;     // attr 0, pix 1 -> color_prom[31]
    v.tile_rom[8] = 0x08;      // tile code 0, pixel (tx=0,ty=0) pen-bit-0 lit (nibble-packed ROM)
    // vram_offset(col=2,row=0) == 64 (MAME pacman_scan_rows decode).
    v.videoram[64] = 0;
    v.colorram[64] = 0;

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.render(out.data());

    EXPECT_NE(out[16 * pacman::kUprightW + 223], 0u) << "expected lit pixel at (223,16)";
    EXPECT_EQ(out[271 * pacman::kUprightW + 0], 0u) << "mirrored-the-other-way location must stay dark";
}

// MAME's draw_sprites (pacman_v.cpp) feeds the *first* $5060-pair register
// (ram2[i]) into sy (sy = ram2[i] - 31) and the *second* (ram2[i+1]) into sx
// (sx = 272 - ram2[i+1]) -- i.e. the register pair's on-screen roles are
// swapped from what their "X-location/Y-location" register names suggest,
// an artifact of these being native (pre-rotation) coordinates. Getting this
// backwards still draws a sprite, just in the wrong place -- e.g. bunched at
// the ghost house instead of each character's real start position.
TEST(Video, SpritePositionMatchesMameRegisterRoles) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;      // attr 0, pix 1 -> color_prom[31]
    v.sprite_rom[8] = 0x08;     // sprite code 0, pixel (px=0,py=0) lit
    v.spriteram[0] = 0;         // code 0, no flip
    v.spriteram[1] = 0;         // color attr 0
    v.sprite_xy[0] = 100;       // ram2[i]     -> sy = 100 - 31 = 69
    v.sprite_xy[1] = 50;        // ram2[i + 1] -> sx = 272 - 50 = 222

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.render(out.data());

    // native (222, 69) rotates (per Rot90MatchesMameFlipXSwapXy) to
    // upright dx=(kVisH-1)-69=154, dy=222.
    EXPECT_NE(out[222 * pacman::kUprightW + 154], 0u) << "expected sprite pixel at native (222,69)";
    // The swapped-registers bug instead lands it at native (69,222) ->
    // upright dx=1, dy=69 -- must stay dark.
    EXPECT_EQ(out[69 * pacman::kUprightW + 1], 0u) << "swapped-register location must stay dark";
}

// Bit 0 of the sprite's first RAM byte is X flip, bit 1 is Y flip -- MAME's
// own `fx = spriteram[offs] & 1` / `fy = spriteram[offs] & 2`. Verified
// against real gameplay in all four joystick directions (mouth must open
// toward the direction of travel, not away from it) and against the ghosts
// (whose flip bits are always 0 -- they must stay dome-up, not render
// upside down). A sprite whose only lit pixel is at local (px=0,py=0) moves
// to local (15,0) when X-flipped: bit 0 set (0x01) must move it, bit 1
// (0x02) must not.
TEST(Video, SpriteFlipBitsAreXBit0YBit1) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;
    v.sprite_rom[8] = 0x08;  // sprite code 0, local pixel (px=0,py=0) lit
    v.sprite_xy[0] = 31;     // sy = 31 - 31 = 0
    v.sprite_xy[1] = 255;    // sx = 272 - 255 = 17 (sprite_xy is uint8_t; 0 isn't reachable)
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};

    v.spriteram[0] = 0x01;   // bit 0 set (X flip), bit 1 clear
    v.spriteram[1] = 0;
    out.fill(0);
    v.render(out.data());
    // Unflipped local (0,0) -> native (17,0); X-flipped -> native (17+15,0).
    // Both rotate (per Rot90MatchesMameFlipXSwapXy) to upright dx=(kVisH-1),
    // dy=native_x.
    EXPECT_EQ(out[17 * pacman::kUprightW + (pacman::kVisH - 1)], 0u)
        << "bit 0 set should X-flip the sprite off native x=17";
    EXPECT_NE(out[32 * pacman::kUprightW + (pacman::kVisH - 1)], 0u)
        << "bit 0 set should X-flip native x=17 to x=32";

    v.spriteram[0] = 0x02;   // bit 1 set (Y flip only) -- X must stay put
    out.fill(0);
    v.render(out.data());
    // Y-flip moves this py=0-only pixel to local py=15, i.e. native y=15,
    // which shifts *dx* (a function of native y) to (kVisH-1)-15=208 -- but
    // dy stays native_x=17, proving X wasn't touched by the Y-flip bit.
    EXPECT_NE(out[17 * pacman::kUprightW + (pacman::kVisH - 1 - 15)], 0u)
        << "bit 1 alone must not X-flip the sprite";
}

// Real hardware's sprite transparency isn't "pen index 0 only" -- MAME's
// draw_sprites resolves it per color group via
// device_palette_interface::transpen_mask(gfx, color, 0), which also treats
// any pen whose looked-up RGB matches pen 0's RGB (for that color) as
// transparent. The ROM leans on this to hide Pac-Man during the "you ate a
// ghost" freeze: it points his sprite at a color whose whole row resolves
// to black, rather than changing his sprite code or position. A renderer
// that only skips literal pen 0 draws that "invisible" Pac-Man as an opaque
// black silhouette, which blots out whatever's underneath it -- in
// practice, part of the eaten ghost's "200"/"400"/etc. score sprite, which
// is drawn at (nearly) the same spot. This is exactly the bug a player sees
// as "the points are cut off, and there's a little black outline of
// Pac-Man" when eating a ghost.
TEST(Video, SpriteWithAllPensMatchingPenZeroDoesNotOccludeSpriteBehindIt) {
    pacman::Video v;
    v.reset();
    // Slot 7 (drawn first, i.e. "underneath"): a normal, visible sprite --
    // color 5's pen 1 resolves to white.
    v.color_prom[31] = 0xFF;
    v.lookup_prom[(5 << 2) | 1] = 31;
    v.sprite_rom[8] = 0x08;  // code 0, local pixel (px=0,py=0) lit (pen 1)
    v.sprite_xy[7 * 2] = 31;        // sy = 31 - 31 = 0
    v.sprite_xy[7 * 2 + 1] = 255;   // sx = 272 - 255 = 17
    v.spriteram[7 * 2] = 0;         // code 0, no flip
    v.spriteram[7 * 2 + 1] = 5;     // color 5

    // Slot 0 (drawn last, i.e. "on top"), same code/position/pixel, but
    // color 6 is left entirely unset -- every pen for it defaults to
    // color_prom[0] (black), same as pen 0 -- exactly the "hide the sprite
    // via color" trick.
    v.sprite_xy[0] = 31;
    v.sprite_xy[1] = 255;
    v.spriteram[0] = 0;             // code 0, no flip
    v.spriteram[1] = 6;             // color 6 -- pens 0 and 1 both resolve to black

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.render(out.data());

    // native (17, 0) rotates to upright dx=(kVisH-1), dy=17.
    EXPECT_EQ(out[17 * pacman::kUprightW + (pacman::kVisH - 1)], 0xFFFFFFu)
        << "the visible sprite underneath must still show through the "
           "'invisible' (all-pens-black) sprite drawn on top of it";
}

// Regression test for a bug where fixing Pac-Man's up/down mouth direction
// (by guessing at bit polarity from left/right behavior alone) accidentally
// flipped every ghost upside down: ghosts always leave both flip bits 0, so
// a sprite whose only lit pixel is at local (px=0,py=15) -- the bottom edge,
// e.g. a ghost's skirt -- must stay at the bottom (dx = 0, the max native-y
// end of the upright screen's horizontal axis) when unflipped.
TEST(Video, UnflippedSpriteKeepsBottomEdgeAtBottom) {
    pacman::Video v;
    v.reset();
    v.color_prom[31] = 0xFF;
    v.lookup_prom[1] = 31;
    v.sprite_rom[8 + 7] = 0x08;  // sprite code 0, local pixel (px=0,py=7) lit (kColByte[0]=8, base_y=7)
    v.sprite_xy[0] = 31 + 7;     // sy = (31+7) - 31 = 7, so this pixel lands at native y=14
    v.sprite_xy[1] = 255;        // sx = 272 - 255 = 17
    v.spriteram[0] = 0;          // no flip -- ghosts always leave this 0
    v.spriteram[1] = 0;
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> out{};
    v.render(out.data());

    // native (17, 14) rotates to upright dx=(kVisH-1)-14=209, dy=17.
    EXPECT_NE(out[17 * pacman::kUprightW + 209], 0u)
        << "unflipped sprite pixel should land at its native position, not be mirrored";
}

TEST(Wsg, SilentWhenDisabled) {
    pacman::Wsg w;
    w.reset();
    w.regs[0x11] = 1;  // ch0 frequency (real map: 0x10 low nibble, 0x11-0x14 rest)
    w.regs[0x15] = 0x0F;  // ch0 volume
    w.wave_prom[0] = 0x0F;
    EXPECT_EQ(w.mix_at(0), 0);
    w.enabled = true;
    EXPECT_NE(w.mix_at(0), 0);
}

// Real hardware register map (MAME src/devices/sound/namco.cpp,
// namco_wsg_device::pacman_sound_w's "pacman register map" comment; matches
// the Midway pacman disassembly's waveform writes to $5045/$504a/$504f and
// its 16-byte $4e8c->$5050 LDIR of freq+volume): only offsets 0x05/0x0a/0x0f
// (waveform), 0x10-0x15 (ch0 freq+vol), 0x16-0x1a (ch1), 0x1b-0x1f (ch2) are
// wired to anything. Offsets 0-4, 6-9, 0x0b-0x0e are dead. A decode that
// instead spaces channels 5 nibbles apart starting at 0 (so channel N's
// frequency and channel N+1's waveform alias the same register) produced
// nothing but crackling noise in practice, since ordinary game writes to one
// voice's field silently corrupted a different voice's field.
TEST(Wsg, RegisterMapMatchesRealHardwareOffsets) {
    pacman::Wsg w;
    w.reset();
    w.enabled = true;
    for (auto& b : w.wave_prom) b = 0x0F;  // max sample everywhere, any voice audible if selected

    // Channel 2 (last voice) lives at 0x1b-0x1e (freq) / 0x1f (vol) / 0x0f
    // (waveform) on real hardware -- NOT at the old buggy 10-14/0x17/0x07.
    w.write(0x1b, 0x01);  // ch2 frequency, low-of-the-4 nibble
    w.write(0x1f, 0x0F);  // ch2 volume
    w.write(0x0f, 0x00);  // ch2 waveform select
    EXPECT_NE(w.mix_at(1u << 20), 0.0f) << "ch2 should be audible from its real-hardware register offsets";

    w.reset();
    w.enabled = true;
    for (auto& b : w.wave_prom) b = 0x0F;
    // These are ch2's *old, wrong* offsets (freq at v*5=0x0a-0x0e, waveform
    // at 5+v=0x07, volume at 0x15+v=0x17) from a decode that spaced channels
    // 5 nibbles apart starting at 0. On real hardware 0x0a is ch1's waveform
    // select and 0x07/0x0b-0x0e are dead, but since ch1's actual volume
    // (0x1a) is never set here, nothing should be audible.
    w.write(0x0a, 0x01);
    w.write(0x17, 0x0F);
    w.write(0x07, 0x00);
    EXPECT_EQ(w.mix_at(1u << 20), 0.0f) << "old wrong per-voice offsets must not make anything audible";
}

// Voice 0 alone has a low frequency nibble (offset 0x10); voices 1 and 2 are
// hardwired with a zero low nibble on real hardware -- a genuine wiring
// quirk (coarser pitch resolution for the two secondary voices), not an
// emulation gap.
TEST(Wsg, OnlyVoiceZeroHasLowFrequencyNibble) {
    pacman::Wsg w;
    w.reset();
    w.enabled = true;
    for (auto& b : w.wave_prom) b = 0x0F;
    w.write(0x15, 0x0F);  // ch0 volume, so ch0 is audible
    w.write(0x10, 0x01);  // ch0's low frequency nibble only
    EXPECT_NE(w.mix_at(1u << 15), 0.0f) << "ch0's low frequency nibble (offset 0x10) should drive its phase";
}
