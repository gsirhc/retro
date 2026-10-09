#include "i8255.h"

namespace galaxian {

void I8255::reset() {
    a = b = c = 0;
    a_in = b_in = cl_in = cu_in = true;
}

uint8_t I8255::read(int port) {
    switch (port & 3) {
        case 0:
            return a_in && in_a ? in_a() : a;
        case 1:
            return b_in && in_b ? in_b() : b;
        case 2: {
            // Each half of C reads its pins as an input, its latch as an output.
            const uint8_t pins = in_c ? in_c() : c;
            const uint8_t lo = cl_in ? pins : c;
            const uint8_t hi = cu_in ? pins : c;
            return uint8_t((lo & 0x0F) | (hi & 0xF0));
        }
        default:
            return 0xFF;
    }
}

void I8255::write(int port, uint8_t v) {
    switch (port & 3) {
        case 0:
            a = v;
            if (!a_in && out_a) out_a(v);
            break;
        case 1:
            b = v;
            if (!b_in && out_b) out_b(v);
            break;
        case 2:
            c = v;
            if ((!cl_in || !cu_in) && out_c) out_c(v);
            break;
        default:
            if ((v & 0x80) == 0) {
                // Bit set/reset on port C (Intel 8255A datasheet).
                const uint8_t bit = uint8_t(1u << ((v >> 1) & 7));
                c = (v & 1) ? uint8_t(c | bit) : uint8_t(c & ~bit);
                if ((!cl_in || !cu_in) && out_c) out_c(c);
                return;
            }
            a_in = (v & 0x10) != 0;
            b_in = (v & 0x02) != 0;
            cl_in = (v & 0x01) != 0;
            cu_in = (v & 0x08) != 0;
            a = b = c = 0;
            break;
    }
}

}  // namespace galaxian
