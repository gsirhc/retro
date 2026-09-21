#include "sound.h"
#include "galaxian/video.h"

#include <algorithm>
#include <cmath>

namespace galaxian {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
// 555 astable f = 1.44/((RA+2RB)*C). FS1 R22=100k R23=470k C=0.01µF, etc.
constexpr double kFsHz[3] = {138.0, 189.0, 267.0};
// HIT: (R35+R36)·C21 = 172k·2.2µF. FIRE shot: R41·C25 = 100k·1µF.
constexpr double kHitTau = 0.378;
constexpr double kFireTau = 0.100;
constexpr double kAudible = 12000.0;

double sq(double phase) { return std::sin(phase) >= 0.0 ? 1.0 : -1.0; }

void wrap(double& phase) {
    if (phase > kTwoPi * 64.0) phase = std::fmod(phase, kTwoPi);
}

}  // namespace

void DiscreteSound::reset() {
    fs[0] = fs[1] = fs[2] = false;
    hit = fire = false;
    vol[0] = vol[1] = false;
    lfo_bits = 0;
    pitch = 0xFF;
    fire_prev_ = false;
    lfo_phase_ = 0;
    fs_phase_[0] = fs_phase_[1] = fs_phase_[2] = 0;
    qa_phase_ = qc_phase_ = qd_phase_ = 0;
    fire_phase_ = 0;
    fire_env_ = 0;
    hit_env_ = 0;
    lfsr_ = 1;
    lfsr_div_ = 0;
}

void DiscreteSound::sound_w(int offset, bool on) {
    switch (offset & 7) {
        case 0: case 1: case 2:
            fs[offset & 7] = on;
            break;
        case 3:
            hit = on;
            break;
        case 5:
            fire = on;
            break;
        case 6:
            vol[0] = on;
            break;
        case 7:
            vol[1] = on;
            break;
        default:
            break;
    }
}

void DiscreteSound::lfo_freq_w(int bit, bool on) {
    uint8_t mask = uint8_t(1 << (bit & 3));
    if (on) lfo_bits |= mask;
    else lfo_bits = uint8_t(lfo_bits & ~mask);
}

void DiscreteSound::pitch_w(uint8_t data) { pitch = data; }

void DiscreteSound::advance(int cpu_cycles) {
    double dt = double(cpu_cycles) / double(kCpuHz);

    double lfo_hz = 0.4 + (double(lfo_bits) / 15.0) * 7.6;
    lfo_phase_ += kTwoPi * lfo_hz * dt;
    wrap(lfo_phase_);
    double lfo = 0.5 + 0.5 * std::sin(lfo_phase_);

    for (int i = 0; i < 3; i++) {
        double hz = kFsHz[i] * (0.75 + 0.5 * lfo);
        fs_phase_[i] += kTwoPi * hz * dt;
        wrap(fs_phase_[i]);
    }

    // Two LS164s: f0 = SOUND_CLOCK / (256 − pitch). 74393 QA/QC/QD = f0/2,/8,/16.
    int div = 256 - int(pitch);
    if (div < 1) div = 1;
    double f0 = kSoundClock / double(div);
    qa_phase_ += kTwoPi * (f0 / 2.0) * dt;
    qc_phase_ += kTwoPi * (f0 / 8.0) * dt;
    qd_phase_ += kTwoPi * (f0 / 16.0) * dt;
    wrap(qa_phase_);
    wrap(qc_phase_);
    wrap(qd_phase_);

    // FIRE 555: rising edge retriggers; C25 keeps decaying after the latch drops.
    if (fire && !fire_prev_) fire_env_ = 1.0;
    fire_prev_ = fire;
    if (fire) fire_env_ = std::max(fire_env_, 0.55);
    fire_env_ *= std::exp(-dt / kFireTau);
    fire_phase_ += kTwoPi * (400.0 + 2200.0 * fire_env_) * dt;
    wrap(fire_phase_);

    lfsr_div_ += cpu_cycles * 2;
    while (lfsr_div_ >= 16) {
        lfsr_div_ -= 16;
        uint32_t bit = ((lfsr_ >> 0) ^ (lfsr_ >> 3)) & 1;
        lfsr_ = (lfsr_ >> 1) | (bit << 16);
    }

    if (hit) hit_env_ = std::min(1.0, hit_env_ + dt * 20.0);
    else hit_env_ *= std::exp(-dt / kHitTau);
}

float DiscreteSound::mix() const {
    float sample = 0;

    if (fs[0]) sample += float(sq(fs_phase_[0])) * 0.16f;
    if (fs[1]) sample += float(sq(fs_phase_[1])) * 0.16f;
    if (fs[2]) sample += float(sq(fs_phase_[2])) * 0.16f;

    int div = 256 - int(pitch);
    if (div < 1) div = 1;
    double f0 = kSoundClock / double(div);
    double qa_hz = f0 / 2.0;
    double qc_hz = f0 / 8.0;
    double qd_hz = f0 / 16.0;
    // Skip harmonics above ~12 kHz so pitch=0xFF (ultrasonic) does not alias.
    if (qa_hz < kAudible) sample += float(sq(qa_phase_)) * 0.12f;
    if (qc_hz < kAudible) sample += float(sq(qc_phase_)) * 0.14f;
    if (vol[0] && qc_hz < kAudible) sample += float(sq(qc_phase_)) * 0.10f;
    if (vol[1] && qd_hz < kAudible) sample += float(sq(qd_phase_)) * 0.12f;

    if (hit_env_ > 0.002) {
        float n = (lfsr_ & 1) ? 1.0f : -1.0f;
        sample += n * 0.40f * float(hit_env_);
    }
    if (fire_env_ > 0.002) {
        float n = (lfsr_ & 2) ? 0.35f : -0.35f;
        sample += (float(sq(fire_phase_)) * 0.45f + n) * float(fire_env_);
    }
    return std::clamp(sample, -1.0f, 1.0f);
}

}  // namespace galaxian
