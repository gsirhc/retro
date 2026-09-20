// Konami Frogger (1981) Galaxian-family video.
//
// Raster is 384×264 at 6.144 MHz (18.432 MHz / 3). Visible 256×224 native,
// then the upright monitor rotates 90° (MAME ROT90 = FLIP_X | SWAP_XY) to
// 224×256. Tilemap is 32×32 of 8×8, 2bpp (607 high plane, 606 low). Object RAM holds
// per-column scroll+colour (the road/river lanes) and eight 16×16 sprites.
// Scroll and sprite-Y enter the adder with nibbles swapped (PCB wiring).
// A hardware colour split paints the river: native x < 128 is blue.
// Color PROM is 32 bytes; Frogger's blue gun has PROM bit 0 unconnected.
// Tile bytes are MSB-left (bit 7 = native left). Color-bit remap is
// schematic-derived; MAME galaxian.cpp / frogger is a cross-check of the
// decode, not a source.

#ifndef FROGGER_VIDEO_H
#define FROGGER_VIDEO_H

#include <array>
#include <cstdint>

namespace frogger {

constexpr int kHTotal = 384;
constexpr int kVTotal = 264;
constexpr int kVisW = 256;   // unrotated visible
constexpr int kVisH = 224;
constexpr int kVisY0 = 16;   // skip two tile rows (Galaxian visarea)
constexpr int kUprightW = 224;
constexpr int kUprightH = 256;
constexpr int kCpuPerLine = kHTotal / 2;                 // 192
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;    // 50688
constexpr int kVBlankLine = kVisY0 + kVisH;              // 240
constexpr int kRiverSplit = 128;                         // native x
constexpr uint32_t kRiverBlue = 0x000047;                // 470 Ω blue gun

class Video {
public:
    std::array<uint8_t, 0x400> videoram{};
    std::array<uint8_t, 0x100> objram{};
    std::array<uint8_t, 0x1000> gfx{};
    std::array<uint8_t, 32> color_prom{};
    bool flip_x = false;
    bool flip_y = false;

    int h = 0, v = 0;
    bool vblank = false;
    bool vblank_edge = false;

    void reset();
    void advance(int cpu_cycles);
    void render(uint32_t* upright_224x256) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t pen) const;
    static uint8_t remap_color(uint8_t attr);

private:
    uint8_t pixel_at(int nx, int ny) const;
};

}  // namespace frogger

#endif
