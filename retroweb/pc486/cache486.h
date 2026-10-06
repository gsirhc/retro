// Memory and I/O timing for the 486 DX2-66 board: the CPU's 8KB L1, a
// 256KB board L2, DRAM, the CPU's write buffers, the VL-Bus video card and
// the ISA bus. Timing only: data always comes from the chipset, so the
// model decides how many core clocks an access costs, never what it reads.
//
// The board is a 1993-94 SiS 85C471-class VLB design (SiS 85C471 data
// sheet, Preliminary V6.0, August 1994):
//   - L1 (Intel486): 8KB unified, 4-way, 16-byte lines, write-through,
//     pseudo-LRU, filled by a 4-dword burst (Embedded Intel486 Processor
//     Hardware Reference Manual, 27302501, "Cache Unit").
//   - Write buffers: four, accepting one write per clock. A full buffer
//     stalls the next write. A read miss may go ahead of buffered writes
//     only when all of them were L1 hits; otherwise they drain first. I/O
//     reads never go ahead, and an OUT waits for the buffer and its own
//     cycle (same manual, "Write Buffers", "I/O Transfers").
//   - L2: 256KB direct-mapped write-back, 16-byte lines, 2-2-2-2 burst
//     read and 2T write, the 471's setting for 20ns SRAM at 33 MHz. A write
//     miss goes to DRAM and leaves the L2 alone (471 "Cache Update Policy").
//   - DRAM: the 471's "Faster" setting for 33 MHz: 4-3-3-3 page-hit burst,
//     3T write. A row miss adds RAS precharge and RAS-to-CAS (3T + 2T).
//   - ISA: 8.33 MHz (bus clock / 4), 1 wait state for 16-bit and 4 for
//     8-bit cycles, half a clock of command delay for I/O, and two clocks
//     of command recovery between cycles (471 "AT Bus State Machine").
//   - VL-Bus VGA, a Cirrus CL-GD5428 (CL-GD542X Technical Reference Manual,
//     SR16 "Performance Tuning"): memory writes take 3 bus clocks ADS# to
//     RDY#, the shortest delay over 3 MCLKs at the default 50.11 MHz MCLK
//     (SR1F); I/O takes the default 2. A read waits for the 7-MCLK RAS
//     cycle, about 140ns, so 6 bus clocks with the address phase. The read
//     figure is derived from the MCLK timing, not printed in the manual.
//   - A read miss stalls the CPU until its first dword arrives; the rest
//     of the line fills behind it, and a later access to that line waits
//     for the fill (Embedded Intel486 Developer's Manual 27302101, 12.3.1).
//   - Turbo off: the 471 holds the CPU off the bus for 4us of every 12us
//     (register 58h bit 4, reset default). Code running from the L1 keeps
//     going at full clock.
//   - DMA: each single-mode 8237 transfer holds the bus for 6 DMA clocks,
//     and a transfer into memory invalidates the L1 line it lands in.
#pragma once

#include <cstdint>
#include <vector>

namespace pc486 {

class Cache486 {
public:
    Cache486();
    void reset();

    // Core clocks per 33 MHz bus clock: the DX2 runs its core at twice the
    // bus.
    static constexpr int kBusRatio = 2;

    bool enabled = false;   // off for a bare CPU; Machine turns it on

    // Stall in core clocks for each kind of access, starting at core clock
    // `now`. `fills` is CR0.CD clear: with CD set the L1 still answers hits
    // but never fills.
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
    // Turbo off on the 471: HOLD for `hold` of every `period` core clocks.
    // Zero turns it off.
    void set_deturbo(uint32_t period, uint32_t hold);
    void invalidate_l1();   // INVD / WBINVD
    void invalidate_l2();   // WBINVD's flush special cycle

    // Exposed for tests.
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

    // Write buffer: completion times of the last four writes, in order.
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
