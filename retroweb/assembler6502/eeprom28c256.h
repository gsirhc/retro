// Atmel/compatible AT28C256 EEPROM model, U2 on the board -- a 32K x 8
// parallel EEPROM in a ZIF socket (Package DIP-28_600_ZIF_ARIES_28-526-10
// per the netlist -- a deliberate hobbyist choice for easy chip-swapping,
// which is why the ROM programmer UI treats this as a socketed chip you
// program out-of-circuit, not something the running CPU pokes at).
//
// Confirmed from pcb6502full.net: U2 pin 27 (~WE) is tied directly to
// +5V -- write-enable is permanently deasserted in-circuit, so the running
// 65C02 can never write this chip over the bus (a store to $8000-$FFFF is
// simply a no-op, like any ROM). The only way to change its contents,
// exactly like the real board, is to pull it and reprogram it externally
// -- modeled here as the `program()` API the ROM programmer UI drives,
// separate from the bus-facing `read()` the running machine uses.
//
// Timing: AT28C256-family parts write in 64-byte pages, one page-write
// cycle (~10ms, datasheet tWC) regardless of how many bytes in the page
// changed. Cite: Atmel AT28C256 datasheet, "Page Write Operation".

#ifndef CG_OAC_6502_EEPROM28C256_H
#define CG_OAC_6502_EEPROM28C256_H

#include <array>
#include <cstdint>

namespace eeprom28c256 {

constexpr int kSize = 32768;
constexpr int kPageSize = 64;
constexpr int kPageWriteCycleUs = 10000;   // datasheet tWC, worst case

class Eeprom {
public:
    uint8_t read(uint16_t addr) const { return data_[addr & (kSize - 1)]; }

    // Begin programming `len` bytes starting at `addr` (spanning as many
    // pages as needed). Real time to completion is
    // pages_touched() * kPageWriteCycleUs, reported by busy(); the
    // programmer UI can poll progress() for an animated page-by-page burn,
    // or advance(really_fast) to skip the wait for the "instant burn" -
    // labelled opt-in (see the ROM programmer's realism rules).
    void begin_program(uint16_t addr, const uint8_t *bytes, int len);
    bool busy() const { return remaining_us_ > 0; }
    // Advance the in-progress burn by `us` microseconds of wall-clock time.
    void advance(int us);
    // 0.0-1.0 across the whole begin_program() call, for a progress bar.
    double progress() const;

    void erase_all() { data_.fill(0xFF); }   // real UV/electrical erase state
    const uint8_t *raw() const { return data_.data(); }
    void load_image(const uint8_t *bytes, int len);   // ships the board's default ROM, or swaps in a saved "chip"

private:
    std::array<uint8_t, kSize> data_{};
    std::array<uint8_t, kSize> pending_{};
    uint16_t pending_start_ = 0;
    int pending_len_ = 0;
    int total_pages_ = 0;
    int pages_done_ = 0;
    int remaining_us_ = 0;
    int us_per_page_ = kPageWriteCycleUs;

    void commit_page(int page_index);
};

} // namespace eeprom28c256

#endif // CG_OAC_6502_EEPROM28C256_H
