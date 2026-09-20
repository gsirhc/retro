// Native end-to-end proof that this machine's VGA Mode 13h and its VESA
// BIOS Extensions actually work -- Milestone 3's equivalent of Milestone
// 1's "boots the real FreeDOS installer to a live C:\>" and Milestone 2's
// pm_stub_check.
//
// Nothing here is asserted by the emulator about itself. A hand-assembled
// 512-byte boot sector is mounted on drive A: and booted by the real,
// freely-licensed BIOS the shipped machine uses, off the real floppy
// controller, exactly like any other DOS boot disk. That guest program:
//
//   1. calls INT 10h AX=4F00 (VBE Return Controller Information) with a
//      "VBE2"-tagged buffer, so the answer comes back as a real VBE 2.0
//      VbeInfoBlock,
//   2. calls AX=4F01 (Return Mode Information) for mode 13h and captures
//      the ModeInfoBlock,
//   3. calls AX=4F02 (Set VBE Mode) with BX=13h -- the actual mode set,
//      done through the VESA interface rather than the legacy AH=00h one,
//   4. calls AX=4F03 (Return Current VBE Mode) and cross-checks it against
//      legacy INT 10h AH=0Fh,
//   5. programs six DAC palette entries (indices 32-37) through the real
//      PEL Address Write / PEL Data ports (0x3C8/0x3C9) and reads one of
//      them straight back through the read side (0x3C7/0x3C9),
//   6. writes an exact pixel pattern into A000:0000 -- corners, centre and
//      a 10-pixel horizontal run on row 50, chosen so a wrong chain-4
//      address decode or a wrong scan-line stride cannot possibly land
//      them all in the right places,
//   7. reads two of those pixels back through the same window (proving the
//      chain-4 read path, not just the write path), and halts.
//
// main() then checks every captured value AND renders the screen through
// the same shared ega_render.cpp the WASM front end uses, asserting the
// exact RGB of each pattern pixel -- the DAC's real 6-bit-per-channel
// values scaled to 8 bits, not a hardcoded palette table.
//
// Usage: vbe_mode13_check <bios> <vgabios> [max_cycles] [out.bmp]
//
// Exits non-zero if any check fails, so it works as a regression test as
// well as a demonstration.

#include "ega_render.h"
#include "machine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// --- guest memory layout the boot sector is built around -----------------
constexpr uint16_t kResults  = 0x0500;  // free low RAM, just above the BDA
constexpr uint16_t kVbeInfo  = 0x1000;  // VbeInfoBlock  (512 bytes) for 4F00
constexpr uint16_t kModeInfo = 0x1200;  // ModeInfoBlock (256 bytes) for 4F01, mode 13h
constexpr uint16_t kSvgaInfo = 0x1400;  // ModeInfoBlock for the VESA-defined mode below
// The lowest VESA-defined 256-color mode, and the only one that fits in
// this card's real 256KB of VRAM (640*400 = 256,000 bytes). See §7.
constexpr uint16_t kSvgaMode = 0x0100;
constexpr uint16_t kBootSeg  = 0x7C00;  // where the BIOS loads us, and our stack top

// Result slots (absolute addresses; DS is 0 throughout the guest).
enum Slot {
    S_VBE_INFO   = kResults + 0x00,  // AX from 4F00
    S_SET_MODE   = kResults + 0x02,  // AX from 4F02
    S_CUR_MODE_AX= kResults + 0x04,  // AX from 4F03
    S_CUR_MODE_BX= kResults + 0x06,  // BX from 4F03 -- the current VBE mode
    S_MODE_INFO  = kResults + 0x08,  // AX from 4F01
    S_DAC_STATE  = kResults + 0x0A,  // 0x3C7 read back (DAC State register)
    S_DAC_R      = kResults + 0x0B,  // entry 32 read back through 0x3C9
    S_DAC_G      = kResults + 0x0C,
    S_DAC_B      = kResults + 0x0D,
    S_PIX_0      = kResults + 0x0E,  // A000:0000 read back
    S_PIX_319    = kResults + 0x0F,  // A000:013F read back
    S_LEGACY_AX  = kResults + 0x10,  // AX from legacy INT 10h AH=0Fh
    S_SVGA_INFO  = kResults + 0x12,  // AX from 4F01 for kSvgaMode
    S_SVGA_SET   = kResults + 0x14,  // AX from 4F02 for kSvgaMode
    S_SVGA_CUR_AX= kResults + 0x16,  // AX from 4F03 while in kSvgaMode
    S_SVGA_CUR_BX= kResults + 0x18,  // BX from 4F03 while in kSvgaMode
    S_SVGA_PIX   = kResults + 0x1A,  // a pixel read back out of the SVGA window
    S_PHASE1     = kResults + 0xFA,  // 0xBEEF once the SVGA screen is painted
    S_GO         = kResults + 0xFC,  // host writes 1 here to release the guest
    S_DONE       = kResults + 0xFE,  // 0xC0DE once the guest is finished
};
constexpr uint16_t kDoneMagic = 0xC0DE;
constexpr uint16_t kPhase1Magic = 0xBEEF;

// --- the pattern the guest paints ----------------------------------------
// Each entry is {x, y, palette index}. The corners pin down both ends of
// the first and last scan line; (160,100) pins the centre; the run on row
// 50 pins the scan-line stride (a wrong stride slides it off row 50).
struct Pixel { int x, y, idx; };
const Pixel kPattern[] = {
    {0,   0,   32}, {319, 0,   33},
    {0,   199, 34}, {319, 199, 35},
    {160, 100, 36},
};
constexpr int kRunRow = 50, kRunX0 = 10, kRunLen = 10, kRunIdx = 37;

// The same idea in the SVGA mode, at 640x400. Every offset here stays
// inside the first 64KB bank (row 102 and below at a 640-byte stride), so
// this is a pure linear-window test with no bank switching -- banking is
// covered in ega_test.cpp instead.
const Pixel kSvgaPattern[] = {
    {0,   0,   64}, {639, 0,   65},
    {0,   100, 66}, {639, 100, 64},
    {320, 50,  65},
};
constexpr int kSvgaRunRow = 20, kSvgaRunX0 = 100, kSvgaRunLen = 10, kSvgaRunIdx = 66;
constexpr int kSvgaWidth = 640, kSvgaHeight = 400;

// The 6-bit-per-channel DAC values the guest programs into entries 32-37,
// in the order the PEL Data register consumes them (R, G, B, auto-
// incrementing to the next entry every third write -- real VGA DAC
// behavior, see ega.h).
constexpr uint8_t kDac[6][3] = {
    {63, 21, 0},   // 32
    {0,  63, 0},   // 33
    {0,  0,  63},  // 34
    {63, 63, 63},  // 35
    {32, 16, 8},   // 36
    {10, 20, 30},  // 37
};
constexpr int kDacFirst = 32;

// Entries 64-66, programmed the same way during the SVGA phase.
constexpr uint8_t kSvgaDac[3][3] = {
    {63, 0,  31},  // 64
    {12, 34, 56},  // 65
    {1,  2,  3},   // 66
};
constexpr int kSvgaDacFirst = 64;

// Real VGA DAC: 6 significant bits per channel driving a full-scale analog
// ramp, so 63 is full brightness. Scaling to 8 bits keeps that full scale
// -- (v<<2)|(v>>4) maps 0->0 and 63->255 exactly. Must match
// ega_render.cpp's DecodeDacColor.
uint8_t Dac8(uint8_t six) { return uint8_t((six << 2) | (six >> 4)); }

// --- a tiny 16-bit real-mode assembler -----------------------------------
struct Asm16 {
    std::vector<uint8_t> b;
    void db(int v) { b.push_back(uint8_t(v)); }
    void db(std::initializer_list<int> vs) { for (int v : vs) db(v); }
    void dw(uint16_t v) { db(v & 0xFF); db((v >> 8) & 0xFF); }

    void cli_()          { db(0xFA); }
    void sti_()          { db(0xFB); }
    void cld_()          { db(0xFC); }
    void hlt_()          { db(0xF4); }
    void xor_ax_ax()     { db({0x31, 0xC0}); }
    void mov_ds_ax()     { db({0x8E, 0xD8}); }
    void mov_es_ax()     { db({0x8E, 0xC0}); }
    void mov_ss_ax()     { db({0x8E, 0xD0}); }
    void mov_ax(uint16_t v) { db(0xB8); dw(v); }
    void mov_bx(uint16_t v) { db(0xBB); dw(v); }
    void mov_cx(uint16_t v) { db(0xB9); dw(v); }
    void mov_dx(uint16_t v) { db(0xBA); dw(v); }
    void mov_sp(uint16_t v) { db(0xBC); dw(v); }
    void mov_di(uint16_t v) { db(0xBF); dw(v); }
    void mov_al(uint8_t v)  { db(0xB0); db(v); }
    void push_ax()       { db(0x50); }
    void push_bx()       { db(0x53); }
    void pop_ax()        { db(0x58); }
    void pop_bx()        { db(0x5B); }
    void int_(uint8_t n) { db(0xCD); db(n); }
    void out_dx_al()     { db(0xEE); }
    void in_al_dx()      { db(0xEC); }
    void stosb()         { db(0xAA); }
    void rep_stosb()     { db({0xF3, 0xAA}); }
    void mov_m16_ax(uint16_t a) { db(0xA3); dw(a); }           // mov [a], ax
    void mov_m16_bx(uint16_t a) { db({0x89, 0x1E}); dw(a); }   // mov [a], bx
    void mov_m8_al(uint16_t a)  { db(0xA2); dw(a); }           // mov [a], al
    void mov_al_es_di()         { db({0x26, 0x8A, 0x05}); }    // mov al, es:[di]
    void mov_m16_imm(uint16_t a, uint16_t v) { db({0xC7, 0x06}); dw(a); dw(v); }
    void jmp_self()      { db({0xEB, 0xFE}); }

    // INT 10h can legitimately return with DS pointing anywhere; restore it
    // to 0 without disturbing the returned AX (and BX, for 4F03) so the
    // stores below land in the results block.
    void restore_ds_keep_ax() { push_ax(); xor_ax_ax(); mov_ds_ax(); pop_ax(); }
    void restore_ds_keep_ax_bx() { push_ax(); push_bx(); xor_ax_ax(); mov_ds_ax(); pop_bx(); pop_ax(); }
    void out_port(uint16_t port, uint8_t v) { mov_dx(port); mov_al(v); out_dx_al(); }
    // do { al = [addr] } while (al == 0) -- 7 bytes, so the backwards
    // branch displacement is a fixed -7.
    void poll_until_nonzero(uint16_t addr) {
        db(0xA0); dw(addr);   // mov al, [addr]
        db({0x08, 0xC0});     // or al, al
        db({0x74, 0xF9});     // jz -7
    }
};

std::vector<uint8_t> BuildBootSector() {
    Asm16 a;
    a.cli_();
    a.xor_ax_ax();
    a.mov_ds_ax();
    a.mov_ss_ax();
    a.mov_sp(kBootSeg);   // stack grows down from just below us
    a.sti_();
    a.cld_();

    // --- 1. VBE 4F00: Return Controller Information -----------------------
    // Tagging the buffer "VBE2" before the call is what the VBE 2.0 spec
    // requires to get the VBE-2-extended block back rather than the 1.x one.
    a.mov_ax(0x4256); a.mov_m16_ax(kVbeInfo + 0);  // 'V','B'
    a.mov_ax(0x3245); a.mov_m16_ax(kVbeInfo + 2);  // 'E','2'
    a.xor_ax_ax(); a.mov_es_ax();
    a.mov_di(kVbeInfo);
    a.mov_ax(0x4F00);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_VBE_INFO);

    // --- 2. VBE 4F01: Return Mode Information for mode 13h ----------------
    a.xor_ax_ax(); a.mov_es_ax();
    a.mov_di(kModeInfo);
    a.mov_cx(0x0013);
    a.mov_ax(0x4F01);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_MODE_INFO);

    // ... and again for a VESA-*defined* mode number. The VBE spec only
    // defines function 01h over its own >= 100h mode numbers; asking about
    // a plain VGA mode number like 13h is outside that, so the two answers
    // are worth capturing separately. See PC486_REVIEW.md §7.
    a.xor_ax_ax(); a.mov_es_ax();
    a.mov_di(kSvgaInfo);
    a.mov_cx(kSvgaMode);
    a.mov_ax(0x4F01);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_SVGA_INFO);

    // --- 2b. Actually enter that VESA mode and paint it --------------------
    // The card advertises this mode, so it has to be able to show it. Same
    // proof shape as the mode-13h phase below, just through the card's
    // linear SVGA window instead of chain-4.
    a.mov_bx(kSvgaMode);
    a.mov_ax(0x4F02);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_SVGA_SET);

    a.mov_ax(0x4F03);
    a.int_(0x10);
    a.restore_ds_keep_ax_bx();
    a.mov_m16_ax(S_SVGA_CUR_AX);
    a.mov_m16_bx(S_SVGA_CUR_BX);

    a.out_port(0x3C8, kSvgaDacFirst);
    a.mov_dx(0x3C9);
    for (const auto &e : kSvgaDac) for (uint8_t c : e) { a.mov_al(c); a.out_dx_al(); }

    a.mov_ax(0xA000);
    a.mov_es_ax();
    for (const Pixel &p : kSvgaPattern) {
        a.mov_di(uint16_t(p.y * kSvgaWidth + p.x));
        a.mov_al(uint8_t(p.idx));
        a.stosb();
    }
    a.mov_di(uint16_t(kSvgaRunRow * kSvgaWidth + kSvgaRunX0));
    a.mov_al(kSvgaRunIdx);
    a.mov_cx(kSvgaRunLen);
    a.rep_stosb();
    a.mov_di(0);
    a.mov_al_es_di();
    a.mov_m8_al(S_SVGA_PIX);

    // Hand the screen to the host to inspect, then wait to be released --
    // the host has to render this frame before the mode-13h set below wipes
    // it. A plain poll, not a HLT, so this can't depend on interrupts.
    a.mov_m16_imm(S_PHASE1, kPhase1Magic);
    a.poll_until_nonzero(S_GO);

    // --- 3. VBE 4F02: Set VBE Mode 13h ------------------------------------
    a.mov_bx(0x0013);
    a.mov_ax(0x4F02);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_SET_MODE);

    // --- 4. VBE 4F03: Return Current VBE Mode, + legacy AH=0Fh cross-check -
    a.mov_ax(0x4F03);
    a.int_(0x10);
    a.restore_ds_keep_ax_bx();
    a.mov_m16_ax(S_CUR_MODE_AX);
    a.mov_m16_bx(S_CUR_MODE_BX);

    a.mov_ax(0x0F00);
    a.int_(0x10);
    a.restore_ds_keep_ax();
    a.mov_m16_ax(S_LEGACY_AX);

    // --- 5. DAC: program entries 32-37 through 0x3C8/0x3C9 ----------------
    // One index write, then 18 data writes -- the real PEL Data register
    // auto-advances R->G->B and on to the next entry, which is exactly how
    // period code loads a whole palette.
    a.out_port(0x3C8, kDacFirst);
    a.mov_dx(0x3C9);
    for (const auto &e : kDac) for (uint8_t c : e) { a.mov_al(c); a.out_dx_al(); }

    // ... and read entry 32 straight back through the read side.
    a.out_port(0x3C7, kDacFirst);
    a.in_al_dx();                    // DX is still 0x3C7: the DAC State register
    a.mov_m8_al(S_DAC_STATE);
    a.mov_dx(0x3C9);
    a.in_al_dx(); a.mov_m8_al(S_DAC_R);
    a.in_al_dx(); a.mov_m8_al(S_DAC_G);
    a.in_al_dx(); a.mov_m8_al(S_DAC_B);

    // --- 6. paint the pattern through the A000 window ---------------------
    a.mov_ax(0xA000);
    a.mov_es_ax();
    for (const Pixel &p : kPattern) {
        a.mov_di(uint16_t(p.y * 320 + p.x));
        a.mov_al(uint8_t(p.idx));
        a.stosb();
    }
    a.mov_di(uint16_t(kRunRow * 320 + kRunX0));
    a.mov_al(kRunIdx);
    a.mov_cx(kRunLen);
    a.rep_stosb();

    // --- 7. read two of them back through the same window -----------------
    a.mov_di(0);
    a.mov_al_es_di();
    a.mov_m8_al(S_PIX_0);
    a.mov_di(319);
    a.mov_al_es_di();
    a.mov_m8_al(S_PIX_319);

    a.mov_m16_imm(S_DONE, kDoneMagic);
    a.cli_();
    a.hlt_();
    a.jmp_self();

    std::vector<uint8_t> sector(512, 0);
    std::printf("guest program: %zu bytes of hand-assembled 16-bit code\n", a.b.size());
    if (a.b.size() > 510) { std::fprintf(stderr, "boot sector overflow: %zu bytes\n", a.b.size()); std::exit(2); }
    std::memcpy(sector.data(), a.b.data(), a.b.size());
    sector[510] = 0x55;
    sector[511] = 0xAA;  // the signature the BIOS insists on before booting us
    return sector;
}

// --- result checking ------------------------------------------------------
int g_failures = 0;
void Check(bool ok, const char *what, long long got, long long want) {
    std::printf("  %-46s %-12lld %s\n", what, got, ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      expected %lld (0x%llX)\n", want, (unsigned long long)want); ++g_failures; }
}
void CheckEq(long long got, long long want, const char *what) { Check(got == want, what, got, want); }

// Checks a rendered frame against the pattern the guest painted into it:
// every pattern pixel must be the exact RGB its DAC entry decodes to, and
// the pixels immediately around the horizontal run must be background. An
// off-by-one scan-line stride, a wrong chain-4 decode or a mis-scaled DAC
// channel all fail here rather than looking "close enough".
void CheckFrame(const pc486::RenderedFrame &f, int w, int h,
                const Pixel *pat, int npat,
                int run_row, int run_x0, int run_len, int run_idx,
                const uint8_t (*dac)[3], int dac_first) {
    CheckEq(f.width, w, "rendered width");
    CheckEq(f.height, h, "rendered height");
    if (f.width != w || f.height != h) return;

    auto pixel_ok = [&](int x, int y, int idx, const char *what) {
        std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
        const uint8_t *e = dac[idx - dac_first];
        uint8_t wr = Dac8(e[0]), wg = Dac8(e[1]), wb = Dac8(e[2]);
        bool ok = f.rgba[i] == wr && f.rgba[i + 1] == wg && f.rgba[i + 2] == wb;
        std::printf("  %-46s RGB(%3u,%3u,%3u) %s\n", what,
                    f.rgba[i], f.rgba[i + 1], f.rgba[i + 2], ok ? "ok" : "FAIL");
        if (!ok) {
            std::printf("      expected RGB(%u,%u,%u) from DAC entry %d = (%u,%u,%u)/63\n",
                        wr, wg, wb, idx, e[0], e[1], e[2]);
            ++g_failures;
        }
    };
    auto black_ok = [&](int x, int y, const char *what) {
        std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
        bool ok = f.rgba[i] == 0 && f.rgba[i + 1] == 0 && f.rgba[i + 2] == 0;
        std::printf("  %-46s RGB(%3u,%3u,%3u) %s\n", what,
                    f.rgba[i], f.rgba[i + 1], f.rgba[i + 2], ok ? "ok" : "FAIL");
        if (!ok) ++g_failures;
    };

    char buf[80];
    for (int n = 0; n < npat; ++n) {
        std::snprintf(buf, sizeof buf, "pixel (%d,%d) = palette %d", pat[n].x, pat[n].y, pat[n].idx);
        pixel_ok(pat[n].x, pat[n].y, pat[n].idx, buf);
    }
    for (int i = 0; i < run_len; ++i) {
        std::snprintf(buf, sizeof buf, "run pixel (%d,%d) = palette %d", run_x0 + i, run_row, run_idx);
        pixel_ok(run_x0 + i, run_row, run_idx, buf);
    }
    black_ok(1, 0, "pixel (1,0) is background (palette 0 = black)");
    black_ok(run_x0 - 1, run_row, "pixel just left of the run is background");
    black_ok(run_x0 + run_len, run_row, "pixel just right of the run is background");
    black_ok(run_x0, run_row - 1, "pixel one row above the run is background");
    black_ok(run_x0, run_row + 1, "pixel one row below the run is background");
}

// Hand-written 24-bit BMP, same helper render_screen.cpp uses.
void WriteBmp(const char *path, int w, int h, const std::vector<uint8_t> &rgba) {
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    uint32_t data_size = uint32_t((row_bytes + pad) * h);
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto w16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    f.put('B'); f.put('M'); w32(54 + data_size); w32(0); w32(54);
    w32(40); w32(uint32_t(w)); w32(uint32_t(h)); w16(1); w16(24); w32(0);
    w32(data_size); w32(2835); w32(2835); w32(0); w32(0);
    std::vector<uint8_t> padbuf(pad, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            f.put(char(rgba[i + 2])); f.put(char(rgba[i + 1])); f.put(char(rgba[i + 0]));
        }
        f.write(reinterpret_cast<const char *>(padbuf.data()), pad);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <bios> <vgabios> [max_cycles] [out.bmp]\n", argv[0]);
        return 2;
    }
    auto bios = ReadFile(argv[1]);
    auto vga = ReadFile(argv[2]);
    if (bios.empty() || vga.empty()) { std::fprintf(stderr, "cannot open/empty BIOS image\n"); return 2; }
    uint64_t budget = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2'000'000'000ull;
    const char *bmp = argc > 4 ? argv[4] : nullptr;

    auto sector = BuildBootSector();

    // A real 1.44MB floppy, our sector first -- the BIOS reads it exactly as
    // it would any DOS boot disk.
    std::vector<uint8_t> floppy(1474560, 0);
    std::memcpy(floppy.data(), sector.data(), sector.size());

    pc486::Machine m;
    m.reset();
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());
    m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    m.chipset.fdc.mount(0, floppy.data(), floppy.size());

    auto rd8  = [&](uint32_t a) { return m.chipset.mem[a]; };
    auto rd16 = [&](uint32_t a) { return uint16_t(m.chipset.mem[a] | (m.chipset.mem[a + 1] << 8)); };
    auto rd32 = [&](uint32_t a) { return uint32_t(rd16(a) | (uint32_t(rd16(a + 2)) << 16)); };

    // The guest paints the SVGA screen, parks on a poll, and waits to be
    // released -- so the frame can be captured before the mode-13h set
    // below wipes it.
    constexpr uint64_t kChunk = 1'000'000;
    bool phase1 = false, done = false;
    pc486::RenderedFrame svga_frame;
    uint64_t phase1_cycles = 0;
    for (uint64_t c = 0; c < budget && !done; c += kChunk) {
        m.run_cycles(kChunk);
        if (!phase1 && rd16(S_PHASE1) == kPhase1Magic) {
            phase1 = true;
            phase1_cycles = m.total_cycles();
            pc486::RenderScreen(m.chipset.vga, svga_frame, /*blink_on=*/true);
            m.chipset.mem[S_GO] = 1;
        }
        done = rd16(S_DONE) == kDoneMagic;
    }
    std::printf("ran %llu cycles, guest %s (SVGA phase captured at %llu)\n",
                (unsigned long long)m.total_cycles(), done ? "finished" : "DID NOT FINISH",
                (unsigned long long)phase1_cycles);
    if (!phase1) { std::printf("FAIL: the guest never reached its SVGA phase marker\n"); return 1; }
    if (!done) { std::printf("FAIL: the guest never reached its completion marker\n"); return 1; }

    std::printf("\n--- VESA BIOS Extensions (INT 10h AX=4F00-4F03) ---\n");
    CheckEq(rd16(S_VBE_INFO), 0x004F, "4F00 Return Controller Information -> AX");
    char sig[5] = {char(rd8(kVbeInfo)), char(rd8(kVbeInfo + 1)), char(rd8(kVbeInfo + 2)), char(rd8(kVbeInfo + 3)), 0};
    Check(std::strcmp(sig, "VESA") == 0, "  VbeInfoBlock.VbeSignature == \"VESA\"", 0, 0);
    std::printf("      signature=\"%s\" version=%u.%u total memory=%u KB\n",
                sig, rd8(kVbeInfo + 5), rd8(kVbeInfo + 4), rd16(kVbeInfo + 18) * 64u);
    Check(rd16(kVbeInfo + 4) >= 0x0102, "  VbeInfoBlock.VbeVersion >= 1.2", rd16(kVbeInfo + 4), 0x0102);
    {
        uint32_t p = rd32(kVbeInfo + 6);
        uint32_t lin = ((p >> 16) << 4) + (p & 0xFFFF);
        std::string oem;
        for (int i = 0; i < 64 && lin + i < m.chipset.mem.size(); ++i) {
            uint8_t ch = rd8(lin + i);
            if (!ch) break;
            oem.push_back(ch >= 32 && ch < 127 ? char(ch) : '.');
        }
        std::printf("      OemStringPtr -> \"%s\"\n", oem.c_str());
        Check(!oem.empty(), "  VbeInfoBlock.OemStringPtr resolves to text", (long long)oem.size(), 1);
    }

    {
        // The mode list function 00h hands back, walked to the 0xFFFF
        // terminator -- these are the modes the card's ROM believes fit in
        // the VRAM the SVGA VideoMemory register reports.
        uint32_t p = rd32(kVbeInfo + 14);
        uint32_t lin = ((p >> 16) << 4) + (p & 0xFFFF);
        std::string modes;
        int n = 0;
        for (; n < 64; ++n) {
            uint16_t mode = rd16(lin + uint32_t(n) * 2);
            if (mode == 0xFFFF) break;
            char b[16];
            std::snprintf(b, sizeof b, "%s%03Xh", n ? " " : "", mode);
            modes += b;
        }
        std::printf("      VideoModePtr -> %d mode(s): %s\n", n, modes.c_str());
        Check(n > 0, "  VbeInfoBlock.VideoModePtr lists at least one mode", n, 1);
    }

    auto check_mode_info = [&](uint16_t base, int w, int h, const char *label) {
        std::printf("      %s ModeAttributes=0x%04X XRes=%u YRes=%u bpp=%u planes=%u model=%u"
                    " bytes/line=%u winA=%04Xh\n",
                    label, rd16(base + 0), rd16(base + 18), rd16(base + 20),
                    rd8(base + 25), rd8(base + 24), rd8(base + 27),
                    rd16(base + 16), rd16(base + 8));
        CheckEq(rd16(base + 18), w, "  ModeInfoBlock.XResolution");
        CheckEq(rd16(base + 20), h, "  ModeInfoBlock.YResolution");
        CheckEq(rd8(base + 25), 8,  "  ModeInfoBlock.BitsPerPixel");
        CheckEq(rd8(base + 24), 1,  "  ModeInfoBlock.NumberOfPlanes");
        CheckEq(rd8(base + 27), 4,  "  ModeInfoBlock.MemoryModel (4 = packed pixel)");
        CheckEq(rd16(base + 16), w, "  ModeInfoBlock.BytesPerScanLine");
        CheckEq(rd16(base + 8), 0xA000, "  ModeInfoBlock.WinASegment");
        Check((rd16(base + 0) & 0x11) == 0x11, "  ModeAttributes: mode supported + graphics",
              rd16(base + 0) & 0x11, 0x11);
    };

    // Function 01h is defined by the VBE spec over its own >= 100h mode
    // numbers. This firmware answers exactly that set and declines a plain
    // VGA mode number -- observed, documented, and asserted here so a
    // future firmware bump that changes it does not slip by unnoticed.
    // See PC486_REVIEW.md §7.
    std::printf("      4F01 for VGA mode number 13h -> AX=0x%04X (firmware answers VESA mode numbers only)\n",
                rd16(S_MODE_INFO));
    CheckEq(rd16(S_MODE_INFO), 0x014F, "4F01 (mode 13h, a VGA mode number) -> AX");
    CheckEq(rd16(S_SVGA_INFO), 0x004F, "4F01 Return Mode Information (mode 100h) -> AX");
    if (rd16(S_SVGA_INFO) == 0x004F) check_mode_info(kSvgaInfo, kSvgaWidth, kSvgaHeight, "100h:");
    CheckEq(rd16(S_SVGA_SET), 0x004F, "4F02 Set VBE Mode (BX=100h) -> AX");
    CheckEq(rd16(S_SVGA_CUR_AX), 0x004F, "4F03 Return Current VBE Mode (in 100h) -> AX");
    CheckEq(rd16(S_SVGA_CUR_BX) & 0x3FFF, kSvgaMode, "4F03 current mode (in 100h) -> BX");
    CheckEq(rd8(S_SVGA_PIX), kSvgaPattern[0].idx, "guest read back A000:0000 in mode 100h");

    std::printf("\n--- rendered SVGA screen, mode %03Xh (captured mid-run) ---\n", kSvgaMode);
    CheckFrame(svga_frame, kSvgaWidth, kSvgaHeight,
               kSvgaPattern, int(sizeof kSvgaPattern / sizeof kSvgaPattern[0]),
               kSvgaRunRow, kSvgaRunX0, kSvgaRunLen, kSvgaRunIdx, kSvgaDac, kSvgaDacFirst);

    CheckEq(rd16(S_SET_MODE), 0x004F, "4F02 Set VBE Mode (BX=13h) -> AX");
    CheckEq(rd16(S_CUR_MODE_AX), 0x004F, "4F03 Return Current VBE Mode -> AX");
    CheckEq(rd16(S_CUR_MODE_BX) & 0x3FFF, 0x0013, "4F03 current mode -> BX");
    CheckEq(rd8(S_LEGACY_AX), 0x13, "legacy INT 10h AH=0Fh cross-check -> AL");

    std::printf("\n--- DAC (ports 0x3C7-0x3C9) ---\n");
    // Writing the PEL Address Read Mode register puts the DAC in read mode;
    // the DAC State register reports that as 3 (11b). See ega.h.
    CheckEq(rd8(S_DAC_STATE) & 0x03, 0x03, "DAC State register reads \"read mode\"");
    CheckEq(rd8(S_DAC_R), kDac[0][0], "entry 32 red read back through 0x3C9");
    CheckEq(rd8(S_DAC_G), kDac[0][1], "entry 32 green read back through 0x3C9");
    CheckEq(rd8(S_DAC_B), kDac[0][2], "entry 32 blue read back through 0x3C9");

    std::printf("\n--- chain-4 video memory at 0xA0000 ---\n");
    CheckEq(rd8(S_PIX_0), 32, "guest read back A000:0000");
    CheckEq(rd8(S_PIX_319), 33, "guest read back A000:013F");

    // What the card's own ROM actually programmed for mode 13h, read back
    // off the live device -- the same "don't guess, read the register"
    // evidence ibmpc-at/IBM_PCAT_REVIEW.md §16 collected for mode 10h.
    {
        pc486::Ega &v = m.chipset.vga;
        std::printf("\n--- what the BIOS programmed (read back off the live card) ---\n");
        std::printf("      CRTC R01 Horizontal Display End = %u\n", v.crtc_horizontal_display_end());
        std::printf("      CRTC R12 Vertical Display End   = %u\n", v.crtc_vertical_display_end());
        std::printf("      CRTC R09 Maximum Scan Line      = %u (scan doubling %s)\n",
                    v.crtc_max_scan_line(), v.crtc_scan_doubling() ? "on" : "off");
        std::printf("      CRTC R13 Offset                 = %u -> %d bytes/scan line (%d-byte address unit)\n",
                    v.crtc_offset(), v.crtc_row_byte_stride(), v.crtc_address_unit_bytes());
        std::printf("      GC   R05 Shift Register field   = %u (2 = 256-color)\n", v.gc_shift_register_mode());
        std::printf("      AC   R10 bit 6 (8-bit color)    = %d\n", int(v.attr_8bit_color()));
        std::printf("      DAC  PEL Mask (0x3C6)           = 0x%02X\n", v.dac_mask());
        std::printf("      SVGA ID register                = 0x%04X, VRAM = %u KB\n",
                    v.vbe_reg(pc486::Ega::kVbeRegId), v.vbe_reg(pc486::Ega::kVbeRegVideoMemory64K) * 64u);
    }

    std::printf("\n--- rendered mode-13h screen (shared ega_render.cpp) ---\n");
    pc486::RenderedFrame frame;
    pc486::RenderScreen(m.chipset.vga, frame, /*blink_on=*/true);
    CheckFrame(frame, 320, 200, kPattern, int(sizeof kPattern / sizeof kPattern[0]),
               kRunRow, kRunX0, kRunLen, kRunIdx, kDac, kDacFirst);

    if (bmp) { WriteBmp(bmp, frame.width, frame.height, frame.rgba); std::printf("\nwrote %s\n", bmp); }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
