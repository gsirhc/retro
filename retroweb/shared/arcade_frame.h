// RGBA frame for the cabinet pages' canvas ImageData.

#ifndef SHARED_ARCADE_FRAME_H
#define SHARED_ARCADE_FRAME_H

#include <emscripten/val.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace arcade {

// A view into wasm memory, valid until the heap next grows; the page copies it at once.
inline emscripten::val rgba_view(const uint32_t* px, size_t count) {
    return emscripten::val(emscripten::typed_memory_view(count * 4, reinterpret_cast<const uint8_t*>(px)));
}

template <int W, int H>
class RgbaFrame {
public:
    uint32_t* clear() {
        rgb_.fill(0);
        return rgb_.data();
    }

    // 0x00RRGGBB in, RGBA bytes out (little-endian wasm).
    emscripten::val rgba() {
        for (size_t i = 0; i < rgb_.size(); i++) {
            uint32_t p = rgb_[i];
            rgba_[i] = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
        }
        return rgba_view(rgba_.data(), rgba_.size());
    }

private:
    std::array<uint32_t, size_t(W) * size_t(H)> rgb_{};
    std::array<uint32_t, size_t(W) * size_t(H)> rgba_{};
};

}  // namespace arcade

#endif
