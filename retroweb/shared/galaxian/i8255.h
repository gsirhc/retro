// Intel 8255 PPI, mode 0 only — the two chips on Konami Galaxian-family
// boards (Frogger, Scramble, …).
//
// Port direction comes from the mode-set control word the boot ROM writes
// (bit 7 = 1). BSR (bit 7 = 0) is unused on these PCBs. Reset leaves every
// port as input, matching the datasheet.

#ifndef GALAXIAN_I8255_H
#define GALAXIAN_I8255_H

#include <cstdint>
#include <functional>

namespace galaxian {

class I8255 {
public:
    std::function<uint8_t()> in_a, in_b, in_c;
    std::function<void(uint8_t)> out_a, out_b, out_c;

    uint8_t a = 0, b = 0, c = 0;
    bool a_in = true, b_in = true, cl_in = true, cu_in = true;

    void reset();
    uint8_t read(int port);            // 0=A 1=B 2=C 3=control
    void write(int port, uint8_t v);
};

}  // namespace galaxian

#endif
