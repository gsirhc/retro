#include <gtest/gtest.h>

#include "galaxian/ay8910.h"
#include "galaxian/konami_sound.h"

#include <cmath>
#include <vector>

namespace {

double konami_rms(bool filtered, int tone_period) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[0] = uint8_t(tone_period);
    ay.regs[7] = 0x3E;
    ay.regs[8] = 0x0F;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    if (filtered) k.filter_w(3 << 6);
    std::vector<float> out;
    k.advance(1789772 / 10, 48000, out);
    double sum = 0;
    for (size_t i = out.size() / 2; i < out.size(); i++) sum += double(out[i]) * out[i];
    return std::sqrt(sum / double(out.size() - out.size() / 2));
}

}  // namespace

TEST(KonamiSound, SwitchedCapsDampTheTone) {
    double dry = konami_rms(false, 32);
    double wet = konami_rms(true, 32);
    EXPECT_GT(dry, 0.05);
    EXPECT_LT(wet, dry * 0.5);
}

TEST(KonamiSound, OutputIsAcCoupled) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x0F;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    std::vector<float> out;
    k.advance(1789772 / 2, 48000, out);
    ASSERT_FALSE(out.empty());
    EXPECT_GT(std::fabs(out.front()), 0.1f);
    EXPECT_LT(std::fabs(out.back()), 1e-3f);
}

TEST(KonamiSound, MuteSilencesTheOutput) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3E;
    ay.regs[8] = 0x0F;
    ay.regs[0] = 32;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    k.mute = true;
    std::vector<float> out;
    k.advance(1789772 / 20, 48000, out);
    for (float s : out) EXPECT_EQ(s, 0.0f);
}

TEST(KonamiSound, SummerGainIsFeedbackOverTheChannelResistors) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.regs[7] = 0x3F;
    ay.regs[8] = 0x0F;
    galaxian::KonamiSound k;
    k.reset();
    k.ay = {&ay, nullptr};
    std::vector<float> out;
    k.advance(64, 0, out);
    // 1 V through 1k + 5.1k into the virtual ground, times the feedback.
    EXPECT_NEAR(k.opamp_volts(), -2200.0 / 6100.0, 1e-6);
    k.feedback_ohms = 5600.0;
    k.advance(64, 0, out);
    EXPECT_NEAR(k.opamp_volts(), -5600.0 / 6100.0, 1e-6);
}
