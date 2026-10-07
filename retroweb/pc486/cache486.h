// Memory and I/O timing for the 486 DX2-66 board. Timing only: data comes
// from the chipset, this decides how many core clocks an access costs.
// SiS 85C471 VLB design (85C471 data sheet, Preliminary V6.0, Aug 1994):
// - L1: 8KB unified, 4-way, 16-byte lines, write-through, pseudo-LRU, 4-dword
//   burst fill (Embedded Intel486 HRM 27302501, "Cache Unit")
// - Write buffers: four, one write per clock. A read miss goes ahead of
//   buffered writes only if all were L1 hits. I/O reads never go ahead, an
//   OUT waits for the buffer ("Write Buffers", "I/O Transfers")
// - L2: 256KB direct-mapped write-back, 2-2-2-2 burst read, 2T write (471 setting
//   for 20ns SRAM at 33 MHz). Write miss goes to DRAM ("Cache Update Policy")
// - DRAM: 471 "Faster" setting: 4-3-3-3 page-hit burst, 3T write, row miss adds
//   3T precharge + 2T RAS-to-CAS
// - ISA: 8.33 MHz (bus clock / 4), 1 wait state 16-bit, 4 for 8-bit, half a
//   clock command delay for I/O, two clocks recovery ("AT Bus State Machine")
// - VL-Bus VGA, Cirrus CL-GD5428 (CL-GD542X TRM, SR16): writes 3 bus clocks
//   ADS# to RDY# (3 MCLKs at 50.11 MHz, SR1F), I/O 2. Reads wait for the 7-MCLK
//   RAS cycle (~140ns), so 6 bus clocks; derived from MCLK, not printed
// - A read miss stalls until the first dword; later access to the line waits
//   for the fill (27302101 12.3.1)
// - Turbo off: 471 holds the CPU off the bus 4us of every 12us (reg 58h bit 4,
//   reset default). Code running from L1 keeps full clock
// - DMA: single-mode 8237 transfer holds the bus 6 DMA clocks, a transfer into
//   memory invalidates its L1 line
#pragma once

#include <cstdint>
#include <vector>

namespace pc486 {

class Cache486 {
public:
    Cache486();
    void reset();

    // Core clocks per 33 MHz bus clock (DX2 core runs at twice the bus)
    static constexpr int kBusRatio = 2;

    bool enabled = false;   // off for a bare CPU; Machine turns it on

    // Stall in core clocks from core clock `now`. `fills` is CR0.CD clear;
    // with CD set the L1 still hits but never fills.
    int read(uint32_t phys, int size, bool fills, uint64_t now) {
        uint32_t line = phys >> 4;
        if (line == last_data_ && ((phys & 15u) + uint32_t(size)) <= 16u) return 0;
        return read_slow(phys, size, fills, now);
    }
    int fetch(uint32_t phys, bool fills, uint64_t now) {
        uint32_t line = phys >> 4;
        if (line == last_code_) return 0;
        return fetch_slow(line, fills, now);
    }
    int write(uint32_t phys, int size, uint64_t now);
    int io(uint16_t port, int size, bool is_write, uint64_t now);
    void dma(uint32_t phys, int size, bool to_mem, uint64_t now);
    // Turbo off on the 471: HOLD for `hold` of every `period` core clocks, 0 = off
    void set_deturbo(uint32_t period, uint32_t hold);
    void invalidate_l1();   // INVD / WBINVD
    void invalidate_l2();   // WBINVD's flush special cycle

    bool l1_has(uint32_t phys) const;
    bool l2_has(uint32_t phys) const;

    static bool is_vga(uint32_t phys) { return phys - 0xA0000u < 0x20000u; }

private:
    static constexpr int kSets = 128;
    static constexpr int kWays = 4;
    static constexpr uint32_t kL2Lines = 256u * 1024u / 16u;

    int read_slow(uint32_t phys, int size, bool fills, uint64_t now);
    int fetch_slow(uint32_t line, bool fills, uint64_t now);
    int read_line(uint32_t line, bool fills, uint64_t now);
    bool l1_lookup(uint32_t line);
    void l1_fill(uint32_t line);
    int bus_read(uint32_t line, bool burst, int &first);
    int bus_write_cost(uint32_t phys);
    int dram_row(uint32_t phys);
    uint64_t held(uint64_t t) const;
    bool settled(uint32_t line, uint64_t t) const;
    int bus_read_at(int first, int total, uint64_t now);

    uint32_t l1_tag_[kSets][kWays];
    uint8_t  l1_plru_[kSets];
    uint32_t last_data_ = ~0u, last_code_ = ~0u;
    std::vector<uint32_t> l2_tag_;   // line + 1, 0 = empty
    std::vector<uint8_t>  l2_dirty_;
    uint32_t dram_row_ = ~0u;

    uint64_t wb_done_[4] = {0, 0, 0, 0};
    int      wb_head_ = 0;
    uint64_t bus_free_ = 0;     // when the bus finishes everything queued
    uint64_t miss_done_ = 0;    // when the last buffered L1-miss write completes
    uint64_t isa_free_ = 0;     // ISA command recovery
    uint64_t read_end_ = 0;     // when the last bus read finishes
    uint32_t fill_line_ = ~0u;  // the L1 line still filling, until fill_done_
    uint64_t fill_done_ = 0;
    uint64_t hold_period_ = 0, hold_len_ = 0;
};

}  // namespace pc486
