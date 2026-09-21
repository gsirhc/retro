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
            uint8_t lo = cl_in && in_c ? uint8_t(in_c() & 0x0F) : uint8_t(c & 0x0F);
            uint8_t hi = cu_in && in_c ? uint8_t(in_c() & 0xF0) : uint8_t(c & 0xF0);
            // When only one half is an input, pull the other half from the
            // last written latch and the input half from the pin callback.
            if (cl_in && cu_in && in_c) return in_c();
            if (!cl_in) lo = uint8_t(c & 0x0F);
            if (!cu_in) hi = uint8_t(c & 0xF0);
            if (cl_in && in_c) lo = uint8_t(in_c() & 0x0F);
            if (cu_in && in_c) hi = uint8_t(in_c() & 0xF0);
            return uint8_t(lo | hi);
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
            if ((v & 0x80) == 0) return;  // BSR unused
            a_in = (v & 0x10) != 0;
            b_in = (v & 0x02) != 0;
            cl_in = (v & 0x01) != 0;
            cu_in = (v & 0x08) != 0;
            a = b = c = 0;
            break;
    }
}

}  // namespace galaxian
