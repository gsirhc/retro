#include "sound.h"

#include <algorithm>
#include <cmath>

namespace asteroids {

namespace {

constexpr double kVcc = 5.0;
constexpr double kRail = 22.0 - 1.5;     // LM324 on +22 V, output high limit
constexpr double kVoh = 3.4, kVol = 0.2;  // LS TTL typical levels
constexpr double k555High = 3.3, k555Low = 0.1;  // NE555 output at 5 V
constexpr double kVbe = 0.65, kVcesat = 0.2;
constexpr double kDiode = 0.6;           // 1N914, small signal
constexpr double kDiodeRef = 0.72;       // CR3, CR4 at about 10 mA through R53
constexpr double kRon = 470;             // 4016 switch
constexpr double kMaster = 12096000.0;
constexpr double k12k = kMaster / 1024, k3k = kMaster / 4096;

// Thump: N7 outputs into Q2's base through R45 R48 R46 R47, R49 + CR2 from
// +5 V, R44 to ground; Q2 R50 charges C33, the 555 empties it through R51.
constexpr double kR49 = 6.8e3, kR44 = 47e3, kR50 = 22e3, kR51 = 18e3, kC33 = 0.22e-6;
constexpr double kThumpR[4] = {220e3, 100e3, 47e3, 22e3};  // D0..D3
constexpr double kR74 = 3.3e3, kC64 = 0.1e-6;

// Saucer: R11 555 with R67 and R69 (|| R68 for the small saucer) on C45,
// Q6 into R62 sets the P11 566's control voltage (R70, C46).
constexpr double kR67 = 5.6e3, kR68 = 15e3, kR69 = 10e3, kC45 = 10e-6;
constexpr double kR64 = 3.9e3, kR62 = 5.6e3, kR70 = 10e3, kC46 = 0.047e-6, k566V = 12.0;
constexpr double k566Tri = 2.4, k566Mid = 6.0;

// Fire: Q4 / Q5 into the 555's discharge pin with R57 / R61 to C35 / C50.
constexpr double kRq4 = 3.3e3, kRdis = 680, kCfire = 1e-6, kCamp = 10e-6;

constexpr double kExpR[4] = {5.6e3, 12e3, 22e3, 47e3};  // EXPAUD0..3 = D5..D2
constexpr double kC24 = 1e-6;

constexpr double kR75 = 2.2e3, kC62 = 1e-6, kR76 = 47e3, kR100 = 1.2e3;
constexpr double kC67 = 0.1e-6, kC68 = 0.1e-6, kR101 = 270e3;
constexpr double kR104 = 6.8e3, kR103 = 10e3, kC69 = 0.1e-6;

// P11 inverting mixer, R86 1K feedback around +5 V.
constexpr double kRf = 1e3;
constexpr double kRthump = 47e3, kRsaucer = 39e3, kRlife = 47e3, kRsfire = 100e3;
constexpr double kRfire = 100e3, kRexp = 4.7e3, kRthrust = 4.7e3;
constexpr double kHpHz = 1.0 / (2 * M_PI * 1e-6 * 10e3);  // C113, R111
constexpr double kFullScale = 0.5;

double ttl(bool b) { return b ? kVoh : kVol; }

double approach(double v, double target, double r, double c, double dt) {
    return v + (target - v) * (1 - std::exp(-dt / (r * c)));
}

}  // namespace

void Sound::reset() {
    explode_ = thump_ = latch_ = 0;
    lfsr_ = 0;
    clk12_ = clk3_ = 0;
    c12_ = c3_ = false;
    p8_ = 0;
    exp_ff_ = false;
    c24_ = kVcc;
    c33_ = 0;
    n8_high_ = false;
    c64_ = kVcc;
    c45_ = 0;
    r10_high_ = false;
    tri_phase_ = 0;
    for (Fire* f : {&saucer_fire_, &ship_}) {
        f->ctrl = kVcc;
        f->cap = 0;
        f->out = false;
        f->amp = kVcc;
        f->node = kVcc;
    }
    n1_ = n3_ = c67_ = vo2_ = 0;
    mix_ = kVcc;
    // Let the caps reach their idle levels, as they do while the tube warms up.
    for (int i = 0; i < int(0.5 / dt_); i++) step();
    hp_ = mix_ - kVcc;
}

void Sound::set_rate(int hz) {
    hz_ = hz > 0 ? hz : 48000;
    sub_ = std::max(1, int(std::ceil(192000.0 / hz_)));
    dt_ = 1.0 / (double(hz_) * sub_);
}

void Sound::write_latch(int q, bool d) {
    if (q < 0 || q > 7) return;
    if (d) latch_ = uint8_t(latch_ | (1u << q));
    else latch_ = uint8_t(latch_ & ~(1u << q));
}

// R10/R9 then P10/P9 LS164; P5 LS86 and N5 LS00 feed back R10 QG xnor P10 QG.
bool Sound::noise() const {
    return ((lfsr_ >> 6) & 1) == ((lfsr_ >> 14) & 1);
}

void Sound::clock12_rise() {
    lfsr_ = uint16_t((lfsr_ << 1) | (noise() ? 1 : 0));
    // P8 LS161 reloads A=P1 B=P0 C=/P0 D=P1+P0: /12, /6, /3, /5.
    if (p8_ == 15) {
        int p1 = (explode_ >> 7) & 1, p0 = (explode_ >> 6) & 1;
        p8_ = p1 | (p0 << 1) | ((p0 ^ 1) << 2) | ((p1 | p0) << 3);
    } else {
        p8_++;
    }
}

double Sound::thump_current() const {
    double g = 1 / kR44, i = 0;
    for (int b = 0; b < 4; b++) {
        g += 1 / kThumpR[b];
        i += ttl((thump_ >> b) & 1) / kThumpR[b];
    }
    double vb = (i + (kVcc - kDiode) / kR49) / (g + 1 / kR49);
    if (vb > kVcc - kDiode) vb = i / g;
    return std::max(0.0, (kVcc - vb - kVbe) / kR50);
}

double Sound::thump_hz() const {
    double i = thump_current();
    if (i <= 0) return 0;
    double floor = i * kR51;
    if (floor >= kVcc / 3) return 0;
    double tc = kC33 * (kVcc / 3) / i;
    double td = kR51 * kC33 * std::log((2 * kVcc / 3 - floor) / (kVcc / 3 - floor));
    return 1 / (tc + td);
}

double Sound::saucer_ic() const {
    double ve = std::max(0.0, c45_ - kVbe);
    double vf = latch(2) ? kDiode : 0.0;
    return std::max(0.0, (ve - vf) / kR64);
}

// NE566: f = 2 (V+ - Vc) / (R1 C1 V+).
double Sound::saucer_hz() const {
    double ve = std::max(0.0, c45_ - kVbe);
    double v5 = std::max(k566V - saucer_ic() * kR62, ve + kVcesat);
    return std::max(0.0, 2 * (k566V - v5) / (kR70 * kC46 * k566V));
}

// Q3 / Q1 ramp C38 / C47 up from +5 V, so Q4 / Q5 feed the 555 less current.
double Sound::fire_current(const Fire& f, double vcap) {
    double active = std::max(0.0, (12.0 - f.ctrl - kVbe) / kRq4);
    double sat = std::max(0.0, (12.0 - kVcesat - vcap) / (kRq4 + kRdis));
    return std::min(active, sat);
}

double Sound::fire_hz(const Fire& f) {
    double i = std::max(0.0, (12.0 - f.ctrl - kVbe) / kRq4);
    if (i <= 0) return 0;
    return 1 / (kCfire * (kVcc / 3) / i + kRdis * kCfire * std::log(2.0));
}

void Sound::step_fire(Fire& f, bool en) {
    double iq = (2 * kDiodeRef - kVbe) / f.rq;
    f.ctrl = std::min(f.ctrl + dt_ * iq / f.cq, 12.0 - iq * f.rq - kVcesat);
    if (!en) {
        f.ctrl = approach(f.ctrl, kVcc, kRon, f.cq, dt_);
        f.amp = approach(f.amp, kVcc, kRon, kCamp, dt_);
        f.out = false;
    }
    if (f.out) {
        f.cap += dt_ * fire_current(f, f.cap) / kCfire;
        if (f.cap >= 2 * kVcc / 3) f.out = false;
    } else {
        f.cap = approach(f.cap, 0, kRdis, kCfire, dt_);
        if (en && f.cap <= kVcc / 3) f.out = true;
    }
    double vout = f.out ? k555High : k555Low;
    double y = (f.amp / f.r_out + kVcc / kRfire) / (1 / f.r_out + 1 / kRfire);
    f.node = std::min(y, vout + kDiode);
    f.amp += dt_ * (f.node - f.amp) / (f.r_out * kCamp);
}

void Sound::step() {
    clk12_ += dt_ * 2 * k12k;
    while (clk12_ >= 1) {
        clk12_ -= 1;
        c12_ = !c12_;
        if (c12_) clock12_rise();
        else if (p8_ == 15) exp_ff_ = noise();  // D6 LS02 clocks R8 LS74 mid-period
    }
    clk3_ += dt_ * 2 * k3k;
    while (clk3_ >= 1) {
        clk3_ -= 1;
        c3_ = !c3_;
    }

    double ix = (kVcc - c24_) / kRexp;
    for (int k = 0; k < 4; k++) {
        bool on = exp_ff_ && ((explode_ >> (5 - k)) & 1);
        ix += (ttl(on) - c24_) / kExpR[k];
    }
    c24_ += dt_ * ix / kC24;

    double ithump = thump_current();
    if (!(thump_ & 0x10)) {
        n8_high_ = false;
        c33_ += dt_ * (ithump - c33_ / kR51) / kC33;
    } else if (n8_high_) {
        c33_ += dt_ * ithump / kC33;
        if (c33_ >= 2 * kVcc / 3) n8_high_ = false;
    } else {
        c33_ += dt_ * (ithump - c33_ / kR51) / kC33;
        if (c33_ <= kVcc / 3) n8_high_ = true;
    }
    double vthump = n8_high_ ? k555High : k555Low;
    c64_ += dt_ * ((vthump - c64_) / kR74 + (kVcc - c64_) / kRthump) / kC64;

    double rb = latch(2) ? kR69 : kR69 * kR68 / (kR69 + kR68);
    if (!latch(0)) {
        c45_ = approach(c45_, 0, kRon, kC45, dt_);
        r10_high_ = true;
    } else if (r10_high_) {
        c45_ = approach(c45_, kVcc, kR67 + rb, kC45, dt_);
        if (c45_ >= 2 * kVcc / 3) r10_high_ = false;
    } else {
        c45_ = approach(c45_, 0, rb, kC45, dt_);
        if (c45_ <= kVcc / 3) r10_high_ = true;
    }
    tri_phase_ += dt_ * saucer_hz();
    tri_phase_ -= std::floor(tri_phase_);
    double saucer = k566Mid + k566Tri / 2 * (4 * std::fabs(tri_phase_ - 0.5) - 1);

    step_fire(saucer_fire_, latch(1));
    step_fire(ship_, latch(4));

    double in = latch(3) ? (ttl(noise()) - kVcc - n1_) / (kR75 + kRon) : 0;
    n1_ += dt_ * (in + (n3_ - n1_) / kR76) / kC62;
    double vo1 = std::clamp(n3_ - c67_, -kVcc, kRail - kVcc);
    double dn3 = -vo1 / (kR101 * kC68);
    c67_ += dt_ * ((n1_ - n3_) / kR76 - n3_ / kR100 + vo1 / kR101) / kC67;
    n3_ += dt_ * dn3;
    vo2_ += dt_ * (-vo1 / kR104 - vo2_ / kR103) / kC69;
    vo2_ = std::clamp(vo2_, -kVcc, kRail - kVcc);

    double life = latch(5) && c3_ ? kVoh : kVol;
    double sum = (c64_ - kVcc) / kRthump + (saucer - kVcc) / kRsaucer + (life - kVcc) / kRlife +
                 (saucer_fire_.node - kVcc) / kRsfire + (ship_.node - kVcc) / kRfire +
                 (c24_ - kVcc) / kRexp + vo2_ / kRthrust;
    mix_ = std::clamp(kVcc - kRf * sum, 0.0, kRail);
}

float Sound::sample() {
    double acc = 0;
    for (int i = 0; i < sub_; i++) {
        step();
        acc += mix_;
    }
    double v = acc / sub_ - kVcc;
    hp_ += (v - hp_) * (1 - std::exp(-2 * M_PI * kHpHz / hz_));
    return float(std::clamp(2 * (v - hp_) / kFullScale, -1.0, 1.0));
}

}  // namespace asteroids
