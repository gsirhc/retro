#include "cache486.h"

#include <algorithm>

namespace pc486 {

namespace {

// Bus clocks (33 MHz). See cache486.h for the sources.
constexpr int kL2Burst = 2 + 2 + 2 + 2;
constexpr int kL2Single = 2;
constexpr int kL2Write = 2;
constexpr int kDramBurst = 4 + 3 + 3 + 3;
constexpr int kDramSingle = 4;
constexpr int kDramWrite = 3;
constexpr int kRowMiss = 3 + 2;         // Trp + Trcd
constexpr int kVlbWrite = 3;
constexpr int kVlbRead = 6;
constexpr int kVlbIo = 2;
constexpr int kIsaClock = 4;            // bus clocks per 8.33 MHz ISA clock
constexpr int kIsa16 = 7 * kIsaClock / 2;   // 3 clocks + 1/2 command delay
constexpr int kIsa8 = 13 * kIsaClock / 2;   // 6 clocks + 1/2 command delay
constexpr int kIsaRecovery = 2 * kIsaClock;
// 8237 runs at half the ISA clock: 5 DMA clocks a transfer (NI AN-011)
// plus the S0 clock that requests HOLD before each single-mode transfer
constexpr int kDmaTransfer = 6 * 2 * kIsaClock;
// Published IN/OUT counts already include one zero-wait bus cycle
constexpr int kIoIncluded = 2;
// The 8-bit tag field of a 256KB L2 covers 64MB (471 "Cache Size Options").
constexpr uint32_t kCacheableTop = 64u * 1024u * 1024u;
// 1Mx4 DRAMs on a 32-bit bank: 1024 columns of 4 bytes per row.
constexpr int kRowShift = 12;

constexpr int T(int bus_clocks) { return bus_clocks * Cache486::kBusRatio; }

bool vlb_port(uint16_t port) { return port >= 0x3B0 && port <= 0x3DF; }
bool isa16_port(uint16_t port) { return port == 0x1F0 || port == 0x170; }

}  // namespace

Cache486::Cache486() : l2_tag_(kL2Lines, 0), l2_dirty_(kL2Lines, 0) { reset(); }

void Cache486::reset() {
    invalidate_l1();
    invalidate_l2();
    dram_row_ = ~0u;
    for (auto &d : wb_done_) d = 0;
    wb_head_ = 0;
    bus_free_ = miss_done_ = isa_free_ = read_end_ = fill_done_ = 0;
    fill_line_ = ~0u;
}

void Cache486::invalidate_l1() {
    for (int s = 0; s < kSets; ++s) {
        for (int w = 0; w < kWays; ++w) l1_tag_[s][w] = 0;
        l1_plru_[s] = 0;
    }
    last_data_ = last_code_ = ~0u;
}

void Cache486::set_deturbo(uint32_t period, uint32_t hold) {
    hold_period_ = hold ? period : 0;
    hold_len_ = hold;
}

// Single-mode DMA transfer. Memory writes are snooped and invalidate
// the L1 line (the 471 drives EADS#).
void Cache486::dma(uint32_t phys, int size, bool to_mem, uint64_t now) {
    if (to_mem) {
        uint32_t last = (phys + uint32_t(size) - 1u) >> 4;
        for (uint32_t line = phys >> 4; line <= last; ++line) {
            uint32_t set = line & (kSets - 1);
            for (int w = 0; w < kWays; ++w)
                if (l1_tag_[set][w] == line + 1) l1_tag_[set][w] = 0;
            if (line == last_data_) last_data_ = ~0u;
            if (line == last_code_) last_code_ = ~0u;
        }
    }
    if (!l2_has(phys)) dram_row(phys);
    uint64_t start = held(std::max(now, bus_free_));
    bus_free_ = start + uint64_t(T(kDmaTransfer));
    read_end_ = bus_free_;   // reads can't go ahead of a DMA cycle
}

void Cache486::invalidate_l2() {
    std::fill(l2_tag_.begin(), l2_tag_.end(), 0u);
    std::fill(l2_dirty_.begin(), l2_dirty_.end(), uint8_t(0));
}

bool Cache486::l1_has(uint32_t phys) const {
    uint32_t line = phys >> 4;
    for (int w = 0; w < kWays; ++w)
        if (l1_tag_[line & (kSets - 1)][w] == line + 1) return true;
    return false;
}

bool Cache486::l2_has(uint32_t phys) const {
    uint32_t line = phys >> 4;
    return l2_tag_[line & (kL2Lines - 1)] == line + 1;
}

// Pseudo-LRU, three bits per set: b0 picks a pair, b1/b2 a way in it
// (Intel486 "Cache Replacement")
bool Cache486::l1_lookup(uint32_t line) {
    uint32_t set = line & (kSets - 1);
    for (int w = 0; w < kWays; ++w) {
        if (l1_tag_[set][w] != line + 1) continue;
        uint8_t &b = l1_plru_[set];
        if (w < 2) b = uint8_t((b | 1u) & ~2u) | uint8_t(w == 0 ? 2u : 0u);
        else b = uint8_t((b & ~1u & ~4u) | (w == 2 ? 4u : 0u));
        return true;
    }
    return false;
}

void Cache486::l1_fill(uint32_t line) {
    uint32_t set = line & (kSets - 1);
    int way = -1;
    for (int w = 0; w < kWays; ++w)
        if (l1_tag_[set][w] == 0) { way = w; break; }
    if (way < 0) {
        uint8_t b = l1_plru_[set];
        way = (b & 1u) ? ((b & 4u) ? 3 : 2) : ((b & 2u) ? 1 : 0);
    }
    uint32_t victim = l1_tag_[set][way];
    if (victim != 0) {
        if (victim - 1 == last_data_) last_data_ = ~0u;
        if (victim - 1 == last_code_) last_code_ = ~0u;
    }
    l1_tag_[set][way] = line + 1;
    l1_lookup(line);
}

int Cache486::dram_row(uint32_t phys) {
    uint32_t row = phys >> kRowShift;
    if (row == dram_row_) return 0;
    dram_row_ = row;
    return kRowMiss;
}

uint64_t Cache486::held(uint64_t t) const {
    if (hold_period_ == 0) return t;
    uint64_t phase = t % hold_period_;
    return phase < hold_len_ ? t - phase + hold_len_ : t;
}

// Returns the stall until the first transfer. A read waits for the write
// buffer unless every buffered write was an L1 hit; then it goes first and
// those writes queue behind it as misses.
int Cache486::bus_read_at(int first, int total, uint64_t now) {
    uint64_t start = std::max(now, read_end_);
    if (bus_free_ > start) {
        if (miss_done_ > start) {
            start = bus_free_;
        } else {
            for (auto &d : wb_done_) if (d > start) d += uint64_t(total);
            bus_free_ += uint64_t(total);
            miss_done_ = bus_free_;
        }
    }
    start = held(start);
    read_end_ = start + uint64_t(total);
    if (bus_free_ < read_end_) bus_free_ = read_end_;
    return int(start + uint64_t(first) - now);
}

// Bus clocks for a line read from L2 or DRAM; `first` is clocks to the first transfer
int Cache486::bus_read(uint32_t line, bool burst, int &first) {
    uint32_t phys = line << 4;
    int rest = burst ? kDramBurst - kDramSingle : 0;
    if (phys >= kCacheableTop) {
        first = kDramSingle + dram_row(phys);
        return first + rest;
    }
    uint32_t idx = line & (kL2Lines - 1);
    if (l2_tag_[idx] == line + 1) {
        first = kL2Single;
        return burst ? kL2Burst : kL2Single;
    }
    first = 0;
    if (l2_tag_[idx] != 0 && l2_dirty_[idx]) {
        first += 4 * kDramWrite + dram_row((l2_tag_[idx] - 1) << 4);
        l2_dirty_[idx] = 0;
    }
    first += kDramSingle + dram_row(phys);
    if (burst) l2_tag_[idx] = line + 1;
    return first + rest;
}

// A miss stalls only until the first dword; an access to the rest of the
// line waits for the fill (27302101 12.3.1, rule 3)
int Cache486::read_line(uint32_t line, bool fills, uint64_t now) {
    if (l1_lookup(line)) return line == fill_line_ && fill_done_ > now ? int(fill_done_ - now) : 0;
    int first;
    int total = bus_read(line, fills, first);
    int stall = bus_read_at(T(first), T(total), now);
    if (fills) {
        l1_fill(line);
        fill_line_ = line;
        fill_done_ = read_end_;
    }
    return stall;
}

bool Cache486::settled(uint32_t line, uint64_t t) const {
    return l1_has(line << 4) && (line != fill_line_ || fill_done_ <= t);
}

int Cache486::read_slow(uint32_t phys, int size, bool fills, uint64_t now) {
    if (is_vga(phys)) return bus_read_at(T(kVlbRead), T(kVlbRead), now);
    uint32_t first = phys >> 4, last = (phys + uint32_t(size) - 1u) >> 4;
    int stall = read_line(first, fills, now);
    if (last != first) stall += read_line(last, fills, now + uint64_t(stall));
    last_data_ = settled(last, now + uint64_t(stall)) ? last : ~0u;
    return stall;
}

int Cache486::fetch_slow(uint32_t line, bool fills, uint64_t now) {
    int stall = is_vga(line << 4) ? bus_read_at(T(kVlbRead), T(kVlbRead), now) : read_line(line, fills, now);
    last_code_ = settled(line, now + uint64_t(stall)) ? line : ~0u;
    return stall;
}

int Cache486::bus_write_cost(uint32_t phys) {
    if (is_vga(phys)) return kVlbWrite;
    if (phys < kCacheableTop) {
        uint32_t line = phys >> 4, idx = line & (kL2Lines - 1);
        if (l2_tag_[idx] == line + 1) { l2_dirty_[idx] = 1; return kL2Write; }
    }
    return kDramWrite + dram_row(phys);
}

// Writes go through the buffer (L1 is write-through, no allocate on
// miss). The writer stalls only when all four entries are pending.
int Cache486::write(uint32_t phys, int size, uint64_t now) {
    bool hit = !is_vga(phys) && l1_lookup(phys >> 4);
    int cycles = int(((phys & 3u) + uint32_t(size) + 3u) >> 2);
    int stall = 0;
    uint64_t t = now;
    if (hit && (phys >> 4) == fill_line_ && fill_done_ > t) {
        stall = int(fill_done_ - t);
        t = fill_done_;
    }
    for (int i = 0; i < cycles; ++i) {
        uint64_t &slot = wb_done_[wb_head_];
        if (slot > t) { stall += int(slot - t); t = slot; }
        uint64_t start = held(std::max(t, bus_free_));
        uint64_t end = start + uint64_t(T(bus_write_cost(phys + uint32_t(4 * i))));
        bus_free_ = end;
        slot = end;
        if (!hit) miss_done_ = end;
        wb_head_ = (wb_head_ + 1) & 3;
    }
    return stall;
}

// Port I/O is unbuffered and in order: waits for the write buffer, and
// the CPU waits for the cycle. Wide accesses take one cycle per device width.
int Cache486::io(uint16_t port, int size, bool is_write, uint64_t now) {
    uint64_t start = held(std::max(now, bus_free_));
    uint64_t end;
    if (vlb_port(port)) {
        end = start + uint64_t(T(kVlbIo));
    } else {
        bool wide = isa16_port(port);
        int cycles = (size == 2 && !wide) ? 2 : 1;
        start = held(std::max(start, isa_free_));
        int each = wide ? kIsa16 : kIsa8;
        end = start + uint64_t(T(cycles * each + (cycles - 1) * kIsaRecovery));
        isa_free_ = end + uint64_t(T(kIsaRecovery));
    }
    bus_free_ = end;
    int stall = int(end - now) - kIoIncluded;
    return stall > 0 ? stall : 0;
}

}  // namespace pc486
