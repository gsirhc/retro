#include <gtest/gtest.h>

#include "machine.h"

#include <array>
#include <cstdint>

namespace {

// OUT 3 / OUT 5 from the reset vector, then spin.
invaders::RomSet outs(uint8_t p3, uint8_t p5) {
    invaders::RomSet s;
    const uint8_t code[] = {0x3E, p3, 0xD3, 0x03, 0x3E, p5, 0xD3, 0x05, 0xC3, 0x08, 0x00};
    std::copy(std::begin(code), std::end(code), s.program.begin());
    return s;
}

}  // namespace

TEST(Machine, IdleInputsMatchThePcb) {
    invaders::Machine m;
    EXPECT_EQ(m.inputs.in0, 0x08);
    EXPECT_EQ(m.inputs.in1 & 0x01, 0x01) << "COIN is active-low";
    EXPECT_EQ(m.inputs.in2, 0x00);
    EXPECT_TRUE(m.watchdog_enabled);
}

TEST(Machine, OutsReachTheMidwaySoundBoard) {
    invaders::Machine m;
    m.load_roms(outs(0x20, 0x20));
    m.reset();
    m.run_cycles(200);
    EXPECT_TRUE(m.sound.amp_enabled());
    EXPECT_TRUE(m.sound.flip_screen());
}

TEST(Machine, AudioIsSampledAtAudioHz) {
    invaders::Machine m;
    m.load_roms(outs(0x00, 0x00));
    m.reset();
    m.run_cycles(invaders::kCpuHz / 10);
    EXPECT_NEAR(double(m.audio.size()), 4800.0, 2.0);
    m.reset();
    EXPECT_TRUE(m.audio.empty());
}

TEST(Machine, RenderIsWhiteOnBlack) {
    invaders::Machine m;
    m.load_roms(outs(0x00, 0x00));
    m.reset();
    m.ram[0x400 + 31] = 0x80;
    m.run_cycles(invaders::kFrameCycles);
    static std::array<uint32_t, invaders::kUprightW * invaders::kUprightH> px;
    m.render(px.data());
    EXPECT_EQ(px[0], 0xFFFFFFu);
    EXPECT_EQ(px[1], 0u);
}
