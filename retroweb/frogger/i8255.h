// Intel 8255 PPI, mode 0 only — the two chips on Konami's 1981 Frogger board.
//
// Port direction comes from the mode-set control word the boot ROM writes
// (bit 7 = 1). BSR (bit 7 = 0) is unused on this PCB. Reset leaves every
// port as input, matching the datasheet.

#ifndef FROGGER_I8255_H
#define FROGGER_I8255_H

#include <cstdint>
#include <functional>

namespace frogger {

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

}  // namespace frogger

#endif
