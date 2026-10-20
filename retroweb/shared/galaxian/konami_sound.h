// Konami sound board AY output stage (Stern Scramble 33A-105, Sega/Gremlin
// Frogger 834-0069): 1k, 4066-switched 0.22/0.047 uF caps and 5.1k into an
// LM741 inverting summer, then 0.15 uF into the power amp.

#ifndef GALAXIAN_KONAMI_SOUND_H
#define GALAXIAN_KONAMI_SOUND_H

#include "ay8910.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaxian {

class KonamiSound {
public:
    // ay[0] takes AV6-AV11 (3D on Scramble), ay[1] AV0-AV5 (3C); Frogger fits only ay[0].
    std::array<Ay8910*, 2> ay{};
    bool mute = false;
    // LM741 feedback: 2.2k on Scramble 33A-105, 5.6k on Frogger 834-0069.
    double feedback_ohms = 2200.0;

    void reset();
    // The latch takes its data from address lines AV0-AV11, not the data bus.
    void filter_w(uint16_t offset);
    // Switch n of 12: AY n / 6, channel (n % 6) / 2, 0.22 uF when even, 0.047 uF when odd.
    bool filter_switch(int n) const { return sw_[unsigned(n)]; }
    void advance(int ay_cycles, int host_hz, std::vector<float>& out);
    // Current output sample, before host resampling.
    float sample() const;
    // Summer output in volts, before the amp coupling.
    double opamp_volts() const { return vout_; }

private:
    struct Channel {
        double vc[2]{};
    };
    std::array<Channel, 6> ch_{};
    std::array<bool, 12> sw_{};
    double vout_ = 0;
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
