// Konami sound board AY output network: 4066-switched RC filters, uA741 mixer, amp coupling (MAME nl_konami.cpp).

#ifndef GALAXIAN_KONAMI_SOUND_H
#define GALAXIAN_KONAMI_SOUND_H

#include "ay8910.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaxian {

class KonamiSound {
public:
    // ay[0] is 3D, ay[1] is 3C; Frogger fits only 3D.
    std::array<Ay8910*, 2> ay{};
    bool mute = false;

    void reset();
    // The latch takes its data from address lines AV0-AV11, not the data bus.
    void filter_w(uint16_t offset);
    // Switch n of 12: AY (n / 6), channel (n % 6) / 2, 0.22 uF when even, 0.047 uF when odd.
    bool filter_switch(int n) const { return sw_[unsigned(n)]; }
    void advance(int ay_cycles, int host_hz, std::vector<float>& out);
    // Current output sample, before host resampling.
    float sample() const { return mute ? 0.0f : float(out_ * kOutScale); }

    static constexpr double kOutScale = 1.0 / 0.05;

private:
    struct Channel {
        double vc[2]{};
    };
    std::array<Channel, 6> ch_{};
    std::array<bool, 12> sw_{};
    double coupling_v_ = 0;
    double out_ = 0;
    int phase_ = 0;
    double host_acc_ = 0;
    double sum_ = 0;
    int n_ = 0;

    void solve();
};

}  // namespace galaxian

#endif
