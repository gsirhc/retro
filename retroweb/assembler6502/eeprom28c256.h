// AT28C256 32K x 8 EEPROM, U2, in a ZIF socket. ~WE (pin 27) is tied to +5V
// per pcb6502full.net, so the CPU can't write it; contents change only by
// reprogramming out-of-circuit via begin_program().
//
// Writes go in 64-byte pages, one ~10ms cycle per page (Atmel AT28C256
// datasheet, "Page Write Operation", tWC).

#ifndef CG_OAC_6502_EEPROM28C256_H
#define CG_OAC_6502_EEPROM28C256_H

#include <array>
#include <cstdint>

namespace eeprom28c256 {

constexpr int kSize = 32768;
constexpr int kPageSize = 64;
constexpr int kPageWriteCycleUs = 10000;

class Eeprom {
public:
    uint8_t read(uint16_t addr) const { return data_[addr & (kSize - 1)]; }

    // Completion takes pages touched * kPageWriteCycleUs; poll busy() and progress().
    void begin_program(uint16_t addr, const uint8_t *bytes, int len);
    bool busy() const { return remaining_us_ > 0; }
    // Advance the burn by `us` microseconds.
    void advance(int us);
    // 0.0-1.0 across the begin_program() call
    double progress() const;

    void erase_all() { data_.fill(0xFF); }
    const uint8_t *raw() const { return data_.data(); }
    void load_image(const uint8_t *bytes, int len);

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
