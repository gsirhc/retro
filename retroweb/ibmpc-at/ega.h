// IBM Enhanced Graphics Adapter -- text-mode subset (Phase 3 scope).
//
// Owns the 256KB video RAM window (0xA0000-0xBFFFF... the card itself only
// decodes 0xA0000-0xAFFFF for graphics and 0xB8000-0xBFFFF for text/CGA
// compatibility; 0xB0000-0xB7FFF is the MDA-compatible mono text window,
// unused by this color-display machine but still decoded so a write there
// doesn't fall through to conventional RAM) and the standard EGA register
// set: CRTC (0x3D4/0x3D5), Sequencer (0x3C4/0x3C5), Graphics Controller
// (0x3CE/0x3CF), Attribute Controller (0x3C0, address/data toggled by
// writes and reset by reading Input Status 1), Input Status 1 (0x3DA),
// and the Miscellaneous Output Register (0x3C2 write / 0x3CC read).
//
// Scope/simplifications (see IBM_PCAT_REVIEW.md):
//  - Text mode only. VRAM is addressed as a flat byte array (character,
//    attribute interleaved, exactly what software sees at 0xB8000 in text
//    mode) rather than the real 4-bitplane odd/even-chained hardware
//    underneath -- a real EGA's Sequencer odd/even chaining mode maps a
//    flat CPU-visible address onto planes 0/1 transparently for text mode,
//    so this is observably correct for text mode specifically. Graphics
//    modes (the real 4-plane latch/Set-Reset/Read-Map-Select machinery)
//    are Phase 5's job.
//  - This machine is EGA + a color display and never operates in
//    monochrome-emulation mode, so the Misc Output Register's I/O-address-
//    select bit is stored and readable but doesn't actually change which
//    port range answers -- the CRTC etc. always respond on the color
//    range (0x3Dx), matching this specific machine's real configuration.
//  - CRTC/Sequencer/Graphics Controller/Attribute Controller registers are
//    stored and readable but mostly not interpreted -- enough to track
//    cursor position and the display start address (useful for a future
//    renderer) without implementing every register's hardware effect.
#ifndef IBMPCAT_EGA_H
#define IBMPCAT_EGA_H

#include <array>
#include <cstdint>

namespace ibmpcat {

class Ega {
public:
    void reset();

    bool owns_port(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    bool owns_mem(uint32_t addr) const { return addr >= 0xA0000 && addr <= 0xBFFFF; }
    uint8_t mem_read(uint32_t addr) const;
    void mem_write(uint32_t addr, uint8_t v);

    // Advances the Input Status 1 retrace toggle against the CPU's running
    // cycle count -- so a BIOS/driver's "wait for vertical retrace" polling
    // loop can't hang. Not a real ~70Hz refresh timing; just enough
    // liveness that the bit visibly changes over a bounded number of ticks.
    void tick(uint64_t cpu_cycles);

    // Host/front-end convenience for a future renderer: current CRTC
    // cursor position and display start address (both are 16-bit CRTC
    // register pairs, offsets into the text-mode VRAM window).
    uint16_t cursor_offset() const { return uint16_t((crtc_[0x0E] << 8) | crtc_[0x0F]); }
    uint16_t start_offset() const { return uint16_t((crtc_[0x0C] << 8) | crtc_[0x0D]); }

    std::array<uint8_t, 256 * 1024> vram{};

private:
    std::array<uint8_t, 25> crtc_{};
    uint8_t crtc_index_ = 0;

    std::array<uint8_t, 5> sequencer_{};
    uint8_t sequencer_index_ = 0;

    std::array<uint8_t, 9> gfx_{};
    uint8_t gfx_index_ = 0;

    std::array<uint8_t, 21> attr_{};
    uint8_t attr_index_ = 0;
    bool attr_flip_flop_addr_ = true;  // true = next 0x3C0 write is an index, false = it's data

    uint8_t misc_output_ = 0;

    bool retrace_ = false;
    uint64_t prev_cycles_ = 0;
    double retrace_credit_ = 0.0;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_H
