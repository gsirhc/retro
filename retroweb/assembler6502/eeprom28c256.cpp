// AT28C256 EEPROM model. See eeprom28c256.h for the interface and citation.

#include "eeprom28c256.h"

#include <algorithm>
#include <cstring>

namespace eeprom28c256 {

void Eeprom::load_image(const uint8_t *bytes, int len) {
    data_.fill(0xFF);
    std::memcpy(data_.data(), bytes, size_t(std::min(len, kSize)));
    pending_len_ = 0; remaining_us_ = 0;
}

void Eeprom::begin_program(uint16_t addr, const uint8_t *bytes, int len) {
    len = std::min(len, kSize);
    pending_start_ = addr;
    pending_len_ = len;
    std::memcpy(pending_.data(), bytes, size_t(len));

    uint16_t first_page = addr / kPageSize;
    uint16_t last_page = uint16_t((addr + (len > 0 ? len - 1 : 0)) / kPageSize);
    total_pages_ = len > 0 ? int(last_page - first_page) + 1 : 0;
    pages_done_ = 0;
    remaining_us_ = total_pages_ * us_per_page_;
}

void Eeprom::advance(int us) {
    if (remaining_us_ <= 0 || total_pages_ == 0) return;
    remaining_us_ -= us;
    int total_us = total_pages_ * us_per_page_;
    int elapsed_us = total_us - std::max(remaining_us_, 0);
    int done = std::min(elapsed_us / us_per_page_, total_pages_);
    uint16_t first_page = pending_start_ / kPageSize;
    for (; pages_done_ < done; pages_done_++) commit_page(first_page + pages_done_);
    if (remaining_us_ <= 0) {
        remaining_us_ = 0;
        for (; pages_done_ < total_pages_; pages_done_++) commit_page(first_page + pages_done_);
    }
}

void Eeprom::commit_page(int page_index) {
    int page_start = page_index * kPageSize;
    for (int i = 0; i < kPageSize; i++) {
        int addr = page_start + i;
        if (addr < pending_start_ || addr >= pending_start_ + pending_len_) continue;
        data_[addr & (kSize - 1)] = pending_[addr - pending_start_];
    }
}

double Eeprom::progress() const {
    if (total_pages_ == 0) return 1.0;
    int total_us = total_pages_ * us_per_page_;
    if (total_us == 0) return 1.0;
    return 1.0 - (double(remaining_us_) / double(total_us));
}

} // namespace eeprom28c256
