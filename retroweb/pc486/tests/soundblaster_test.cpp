// GoogleTest suite for the Sound Blaster 16: port decode, DSP reset and identification, 8-bit and
// DSP 4.xx playback, paced DMA handoff, interrupt status and the CT1745 mixer.
// Checked against Creative's Sound Blaster Series Hardware Programming Guide (SBPG).

#include <gtest/gtest.h>

#include "soundblaster.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using pc486::SoundBlaster;

// This machine's real clock, the rate tick() is fed (machine.h's kCpuHz).
constexpr double kCpuHz = 66000000.0;

constexpr uint16_t kBase = 0x220;
constexpr uint16_t kMixerAddr = kBase + 0x04;
constexpr uint16_t kMixerData = kBase + 0x05;
constexpr uint16_t kReset = kBase + 0x06;
constexpr uint16_t kReadData = kBase + 0x0A;
constexpr uint16_t kWriteCmd = kBase + 0x0C;
constexpr uint16_t kWriteStatus = kBase + 0x0C;
constexpr uint16_t kReadStatus = kBase + 0x0E;
constexpr uint16_t kAck16 = kBase + 0x0F;

class SoundBlasterTest : public ::testing::Test {
protected:
    SoundBlaster sb;
    uint64_t cycles_ = 0;

    void SetUp() override { sb.reset(); }

    // SBPG 2-2: write 1 to the reset port, wait, write 0, poll Read-Buffer Status, read 0AAh.
    void ResetDsp() {
        sb.out(kReset, 1);
        sb.out(kReset, 0);
        ASSERT_TRUE(sb.in(kReadStatus) & 0x80) << "no acknowledge byte waiting after reset";
        ASSERT_EQ(sb.in(kReadData), 0xAA);
    }

    // SBPG 2-4: poll Write-Buffer Status for bit 7 clear, then write. Every DSP write goes through here.
    void Write(uint8_t v) {
        ASSERT_EQ(sb.in(kWriteStatus) & 0x80, 0x00) << "DSP reported busy";
        sb.out(kWriteCmd, v);
    }
    void Cmd(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) Write(b);
    }
    uint8_t Read() {
        EXPECT_TRUE(sb.in(kReadStatus) & 0x80);
        return sb.in(kReadData);
    }

    uint8_t MixerRead(uint8_t index) {
        sb.out(kMixerAddr, index);
        return sb.in(kMixerData);
    }
    void MixerWrite(uint8_t index, uint8_t v) {
        sb.out(kMixerAddr, index);
        sb.out(kMixerData, v);
    }

    void Tick(uint64_t delta) {
        cycles_ += delta;
        sb.tick(cycles_);
    }

    // Step time until the card asserts DMA request; false if it never does.
    bool RunUntilTransfer(uint64_t step = 512, int max_steps = 4000000) {
        for (int i = 0; i < max_steps && !sb.transfer_ready(); ++i) Tick(step);
        return sb.transfer_ready();
    }

    // The chipset's part for one burst: fill the buffer from `data` (playback) or drop it (record),
    // then complete the transfer. Returns bytes moved.
    std::size_t ServeBurst(const std::vector<uint8_t> &data, std::size_t &offset) {
        std::size_t len = sb.transfer_length();
        if (!sb.transfer_is_input()) {
            uint8_t *buf = sb.transfer_buffer();
            for (std::size_t i = 0; i < len; ++i) buf[i] = data[(offset + i) % data.size()];
        }
        offset += len;
        sb.finish_transfer(len);
        return len;
    }
};

// --- port decode ---------------------------------------------------------

TEST_F(SoundBlasterTest, DecodesTheWholeTwentyPortBlockPlusTheAdLibFmPair) {
    // SBPG Appendix A, Table A-15: base+0h..13h, twenty ports; base+10h-13h is the card's own CD-ROM interface.
    for (uint16_t p = 0x220; p <= 0x233; ++p) EXPECT_TRUE(sb.owns(p)) << std::hex << p;
    EXPECT_FALSE(sb.owns(0x21F));
    EXPECT_FALSE(sb.owns(0x234));
    // Plus the AdLib FM pair every Sound Blaster answers.
    EXPECT_TRUE(sb.owns(0x388));
    EXPECT_TRUE(sb.owns(0x389));
}

TEST_F(SoundBlasterTest, CdRomInterfacePortsAreOpenBusWithNoDriveAttached) {
    // The CD-ROM is on the ATAPI channel; base+10h-13h is decoded with no drive behind it (reads high, writes vanish).
    for (uint16_t p = 0x230; p <= 0x233; ++p) {
        EXPECT_EQ(sb.in(p), 0xFF) << std::hex << p;
        sb.out(p, 0x00);           // must not disturb anything else
        EXPECT_EQ(sb.in(p), 0xFF) << std::hex << p;
    }
}

TEST_F(SoundBlasterTest, CdGainFollowsMixerVolumeAndIsGatedByTheOutputSwitch) {
    // Mixer 36h/37h (CD volume) feed the same 5-bit attenuator as Voice/Line, gated by 3Ch CD.L/CD.R
    // (bits 2/1). Default volume is near-silent (-62 dB) but the switches default closed.
    EXPECT_NEAR(sb.cd_gain_left(), 0.0f, 0.01f);
    EXPECT_NEAR(sb.cd_gain_right(), 0.0f, 0.01f);
    MixerWrite(0x30, 31 << 3);  // Master L: level 31 -> 0 dB, so only the CD leg is under test
    MixerWrite(0x31, 31 << 3);  // Master R: ditto
    MixerWrite(0x36, 31 << 3);  // CD volume L: level 31 -> 0 dB
    MixerWrite(0x37, 31 << 3);  // CD volume R: ditto
    EXPECT_NEAR(sb.cd_gain_left(), 1.0f, 0.01f);
    EXPECT_NEAR(sb.cd_gain_right(), 1.0f, 0.01f);
    MixerWrite(0x3C, 0x00);  // open every output switch
    EXPECT_FLOAT_EQ(sb.cd_gain_left(), 0.0f) << "CD.L switch open -- the port is disconnected";
    EXPECT_FLOAT_EQ(sb.cd_gain_right(), 0.0f) << "CD.R switch open";
}

// --- DSP reset / identification ------------------------------------------

TEST_F(SoundBlasterTest, ResetPostsTheAcknowledgeByteOnlyOnTheOneToZeroTransition) {
    // SBPG 2-2: the sequence is 1 then 0; a card parked in reset posts nothing.
    sb.out(kReset, 1);
    EXPECT_EQ(sb.in(kReadStatus) & 0x80, 0x00);
    sb.out(kReset, 0);
    ASSERT_TRUE(sb.in(kReadStatus) & 0x80);
    EXPECT_EQ(sb.in(kReadData), 0xAA);
    // FIFO drained: the status bit drops.
    EXPECT_EQ(sb.in(kReadStatus) & 0x80, 0x00);
}

TEST_F(SoundBlasterTest, DrainedReadFifoKeepsReturningTheLastByte) {
    ResetDsp();
    // Real hardware re-presents the last byte read.
    EXPECT_EQ(sb.in(kReadData), 0xAA);
    EXPECT_EQ(sb.in(kReadData), 0xAA);
}

TEST_F(SoundBlasterTest, VersionQueryReportsFourPointOhFiveMajorThenMinor) {
    ResetDsp();
    Cmd({0xE1});
    uint8_t major = Read();
    uint8_t minor = Read();
    EXPECT_EQ(major, 4);
    EXPECT_EQ(minor, 5);
    // Allegro 3.x sb.c builds (major << 8) | minor and takes its SB16 path only at >= 0x400. See soundblaster.h.
    EXPECT_GE((major << 8) | minor, 0x400);
}

TEST_F(SoundBlasterTest, IdentificationCommandReturnsTheComplementOfItsParameter) {
    ResetDsp();
    Cmd({0xE0, 0x5A});
    EXPECT_EQ(Read(), uint8_t(~0x5A));
}

TEST_F(SoundBlasterTest, CopyrightStringIsReturnedNullTerminated) {
    ResetDsp();
    Cmd({0xE3});
    std::string s;
    for (int i = 0; i < 64; ++i) {
        uint8_t c = sb.in(kReadData);
        if (c == 0) break;
        s.push_back(char(c));
    }
    // Drivers use this to tell a Creative card from a clone.
    EXPECT_EQ(s, "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.");
}

TEST_F(SoundBlasterTest, SpeakerFlagFollowsD1AndD3AndIsReportedByD8) {
    ResetDsp();
    Cmd({0xD8});
    EXPECT_EQ(Read(), 0x00);  // off out of reset
    Cmd({0xD1});
    Cmd({0xD8});
    EXPECT_EQ(Read(), 0xFF);  // SBPG: FFh speaker on, 00h off
    EXPECT_TRUE(sb.speaker_on());
    Cmd({0xD3});
    Cmd({0xD8});
    EXPECT_EQ(Read(), 0x00);
}

TEST_F(SoundBlasterTest, TestRegisterSurvivesADspReset) {
    ResetDsp();
    Cmd({0xE4, 0x3C});
    Cmd({0xE8});
    EXPECT_EQ(Read(), 0x3C);
    ResetDsp();  // "DSP reset does not clear the test register"
    Cmd({0xE8});
    EXPECT_EQ(Read(), 0x3C);
}

TEST_F(SoundBlasterTest, DiagnosticIrqCommandsSetTheirOwnStatusBitAndAckPorts) {
    ResetDsp();
    // F2h/F3h are how a driver's IRQ auto-detection finds the programmed line.
    Cmd({0xF2});
    EXPECT_TRUE(sb.irq_pending());
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);  // 8-bit source (SBPG 2-5)
    sb.in(kAck16);                            // wrong port: does not clear it
    EXPECT_TRUE(sb.irq_pending());
    sb.in(kReadStatus);                       // base+Eh acknowledges 8-bit
    EXPECT_FALSE(sb.irq_pending());

    Cmd({0xF3});
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x02);  // 16-bit source
    sb.in(kReadStatus);
    EXPECT_TRUE(sb.irq_pending());
    sb.in(kAck16);                            // base+Fh acknowledges 16-bit
    EXPECT_FALSE(sb.irq_pending());
}

// --- sampling rate -------------------------------------------------------

TEST_F(SoundBlasterTest, TimeConstantAndDirectRateBothProgramTheSampleRate) {
    ResetDsp();
    // SBPG chapter 3: only the high byte of 65536 - 256000000/(channels*rate) is programmed.
    // 166 is 11025 Hz and the card clocks 1000000/90 Hz.
    Cmd({0x40, 166});
    EXPECT_EQ(sb.sample_rate_hz(), 1000000u / 90u);
    // 41h carries the rate in Hz high byte first, unlike every length parameter (SBPG 3-26).
    // 22050 Hz is 5622h; swapped would be 2256h (8790 Hz).
    Cmd({0x41, 0x56, 0x22});
    EXPECT_EQ(sb.sample_rate_hz(), 22050u);
    Cmd({0x41, 0xAC, 0x44});
    EXPECT_EQ(sb.sample_rate_hz(), 44100u);   // SBPG's own worked example
}

TEST_F(SoundBlasterTest, SamplingRateClampsToWhatTheDspCanClockAtAll) {
    // The rate registers hold 4000 Hz (lowest in SBPG Tables 3-2/3-3, ADPCM) to 44100.
    // The format-dependent limit applies when a transfer starts (next two tests).
    ResetDsp();
    Cmd({0x41, 0x00, 0x00});  // 0 Hz requested
    EXPECT_EQ(sb.sample_rate_hz(), 4000u);
    Cmd({0x41, 0xFF, 0xFF});  // 65535 Hz requested
    EXPECT_EQ(sb.sample_rate_hz(), 44100u);
    // The 40h time-constant path reaches far outside the range: tc 0 is ~3906 Hz, tc 255 is 1 MHz.
    Cmd({0x40, 0});
    EXPECT_EQ(sb.sample_rate_hz(), 4000u);
    Cmd({0x40, 255});
    EXPECT_EQ(sb.sample_rate_hz(), 44100u);
}

TEST_F(SoundBlasterTest, StartingAPcmTransferImposesTheFiveKFloor) {
    // SBPG Tables 3-2/3-3: every PCM format on DSP 4.xx runs 5000-44100 Hz.
    ResetDsp();
    Cmd({0x41, 0x00, 0x00});          // asks for 0 Hz
    Cmd({0xC0, 0x00, 0x0F, 0x00});    // 8-bit mono unsigned, 16 bytes
    EXPECT_EQ(sb.sample_rate_hz(), 5000u);
}

TEST_F(SoundBlasterTest, StartingAnAdpcmTransferImposesThatRatiosOwnCeiling) {
    // Table 3-2 ADPCM rows: 4-bit up to 12000 Hz, 3-bit 13000, 2-bit 11000, all from 4000.
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});   // asks for 44100 Hz
    Cmd({0x75, 0x0F, 0x00});   // 4-bit ADPCM with reference
    EXPECT_EQ(sb.sample_rate_hz(), 12000u);
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});
    Cmd({0x77, 0x0F, 0x00});   // 3-bit (2.6-bit) with reference
    EXPECT_EQ(sb.sample_rate_hz(), 13000u);
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});
    Cmd({0x17, 0x0F, 0x00});   // 2-bit with reference
    EXPECT_EQ(sb.sample_rate_hz(), 11000u);
}

// --- legacy 8-bit playback ----------------------------------------------

TEST_F(SoundBlasterTest, LegacySingleCycleOutputMovesUnsignedMonoBytesOnDmaChannelOne) {
    ResetDsp();
    Cmd({0xD1});        // speaker on
    Cmd({0x40, 166});   // ~11 kHz, the rate DOOM-era digitized effects use
    // 14h + length-1: eight bytes (SBPG 3-13).
    Cmd({0x14, 0x07, 0x00});

    ASSERT_TRUE(sb.playing());
    EXPECT_EQ(sb.transfer_dma_channel(), 1);
    EXPECT_FALSE(sb.transfer_is_16bit());
    EXPECT_FALSE(sb.transfer_is_input());
    EXPECT_FALSE(sb.transfer_is_autoinit());

    // 80h is silence for unsigned 8-bit data.
    std::vector<uint8_t> pcm = {0x80, 0xFF, 0x00, 0x80, 0xC0, 0x40, 0x80, 0x80};
    std::size_t offset = 0, moved = 0;
    for (int guard = 0; guard < 64 && sb.playing(); ++guard) {
        ASSERT_TRUE(RunUntilTransfer());
        moved += ServeBurst(pcm, offset);
    }
    EXPECT_EQ(moved, 8u);
    EXPECT_FALSE(sb.playing());  // single-cycle: stops at the end of the block

    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), 8u);
    EXPECT_EQ(samples[0].left, 0);            // 80h unsigned == silence
    EXPECT_EQ(samples[0].left, samples[0].right);  // mono duplicated to both channels
    EXPECT_EQ(samples[1].left, 0x7F00);       // FFh: (255-128) scaled up by 256
    EXPECT_EQ(samples[2].left, -32768);       // 00h: full negative
    // Timestamps advance in order at the programmed rate.
    EXPECT_LT(samples[0].cpu_cycle, samples[7].cpu_cycle);

    // End of block raises the 8-bit interrupt, reported through mixer 82h.
    EXPECT_TRUE(sb.irq_pending());
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);
    sb.in(kReadStatus);
    EXPECT_FALSE(sb.irq_pending());
}

TEST_F(SoundBlasterTest, PlaybackIsPacedAtTheRealSampleRateAndNeverFaster) {
    // 256 samples at 11 kHz must take the wall-clock time they take on a real card.
    ResetDsp();
    Cmd({0x40, 166});
    const uint32_t rate = sb.sample_rate_hz();
    Cmd({0x14, 0xFF, 0x00});  // 256 bytes

    std::vector<uint8_t> pcm(256, 0x80);
    std::size_t offset = 0;
    const uint64_t start = cycles_;
    while (sb.playing()) {
        ASSERT_TRUE(RunUntilTransfer());
        ServeBurst(pcm, offset);
    }
    const double elapsed_cycles = double(cycles_ - start);
    const double expected = 256.0 / double(rate) * kCpuHz;
    EXPECT_GE(elapsed_cycles, expected * 0.99);
    EXPECT_LE(elapsed_cycles, expected * 1.05);
}

TEST_F(SoundBlasterTest, LongBlocksArriveAsMultipleSubMillisecondBurstsNotOneBigOne) {
    // PC486_REVIEW.md DMA-timing item: a burst's bytes move at the end of the window they cover,
    // so the window is capped at ~1 ms rather than moving the whole 256KB buffer in one step.
    ResetDsp();
    Cmd({0x40, 166});
    const uint32_t rate = sb.sample_rate_hz();
    const std::size_t block = 256;
    Cmd({0x14, uint8_t((block - 1) & 0xFF), uint8_t((block - 1) >> 8)});

    std::vector<uint8_t> pcm(block, 0x80);
    std::size_t offset = 0, moved = 0;
    int bursts = 0;
    const std::size_t max_burst_bytes = std::max<std::size_t>(1, rate / 1000);
    while (sb.playing() && bursts < 1000) {
        ASSERT_TRUE(RunUntilTransfer());
        const std::size_t len = sb.transfer_length();
        EXPECT_LE(len, max_burst_bytes) << "burst " << bursts << " exceeded the ~1ms cap";
        EXPECT_GT(len, 0u);
        moved += ServeBurst(pcm, offset);
        ++bursts;
    }
    EXPECT_EQ(moved, block);
    EXPECT_GT(bursts, 4) << "a " << block << "-byte block at ~" << rate
                          << "Hz should need several sub-ms bursts, not one";
}

TEST_F(SoundBlasterTest, AutoInitPlaybackRepeatsBlocksUntilTheExitCommand) {
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x48, 0x03, 0x00});  // block transfer size: 4 bytes (SBPG 3-15)
    Cmd({0x1C});              // 8-bit auto-init output
    EXPECT_TRUE(sb.transfer_is_autoinit());

    std::vector<uint8_t> pcm = {0x80, 0x90, 0xA0, 0xB0};
    std::size_t offset = 0;
    int blocks = 0;
    for (int i = 0; i < 6; ++i) {
        std::size_t moved = 0;
        while (moved < 4) {
            ASSERT_TRUE(RunUntilTransfer());
            moved += ServeBurst(pcm, offset);
        }
        ++blocks;
        // Each completed block interrupts; the driver refills and acks.
        EXPECT_TRUE(sb.irq_pending()) << "block " << blocks;
        sb.in(kReadStatus);
        EXPECT_TRUE(sb.playing()) << "auto-init must not stop on its own";
    }
    EXPECT_EQ(blocks, 6);

    // DAh exits at the end of the current block, not immediately.
    Cmd({0xDA});
    EXPECT_TRUE(sb.playing());
    std::size_t moved = 0;
    while (moved < 4) {
        ASSERT_TRUE(RunUntilTransfer());
        moved += ServeBurst(pcm, offset);
    }
    EXPECT_FALSE(sb.playing());
}

TEST_F(SoundBlasterTest, PauseAndContinueOnlyActOnTheMatchingTransferWidth) {
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x14, 0xFF, 0x00});
    ASSERT_TRUE(RunUntilTransfer());
    std::vector<uint8_t> pcm(256, 0x80);
    std::size_t offset = 0;
    ServeBurst(pcm, offset);

    Cmd({0xD0});  // pause 8-bit DMA
    EXPECT_FALSE(sb.playing());
    EXPECT_FALSE(RunUntilTransfer(512, 4000)) << "a paused card must stop requesting bytes";

    // D6h continues 16-bit transfers; a real card ignores it here.
    Cmd({0xD6});
    EXPECT_FALSE(sb.playing());

    Cmd({0xD4});  // continue 8-bit DMA
    EXPECT_TRUE(sb.playing());
    EXPECT_TRUE(RunUntilTransfer());
}

// --- FM (OPL3) through the card's own ports ------------------------------

// A driver must be able to find the OPL3 through the card's port block: the AdLib detection
// sequence against base+8h/9h, the alias every period program uses.
TEST_F(SoundBlasterTest, AdLibDetectionSucceedsThroughTheFmPorts) {
    const uint16_t kFmAddr = kBase + 0x08, kFmData = kBase + 0x09;
    auto fm_write = [&](uint8_t reg, uint8_t v) {
        sb.out(kFmAddr, reg);
        sb.out(kFmData, v);
    };
    fm_write(0x04, 0x60);  // mask and reset both timers
    fm_write(0x04, 0x80);  // reset the IRQ flags
    EXPECT_EQ(sb.in(kFmAddr), 0x00) << "a quiet OPL3 must read back 00h";

    fm_write(0x02, 0xFF);  // timer 1 preset: expires after one 80.8 us tick
    fm_write(0x04, 0x21);  // mask timer 2, start timer 1
    Tick(uint64_t(kCpuHz * 200e-6));
    EXPECT_EQ(sb.in(kFmAddr), 0xC0) << "timer 1 expired: IRQ + timer-1 flag";
    // base+0h and base+2h are one status register.
    EXPECT_EQ(sb.in(kBase + 0x00), 0xC0);
    EXPECT_EQ(sb.in(kBase + 0x02), 0xC0);

    fm_write(0x04, 0x60);
    fm_write(0x04, 0x80);
    EXPECT_EQ(sb.in(kFmAddr), 0x00);
}

// 0x388/0x389 is the original AdLib FM pair. AdLib-era music drivers write FM registers here
// and never touch the card's own block.
TEST_F(SoundBlasterTest, AdLibDetectionSucceedsThroughThe388Pair) {
    auto fm_write = [&](uint8_t reg, uint8_t v) {
        sb.out(SoundBlaster::kAdLibFmAddr, reg);
        sb.out(SoundBlaster::kAdLibFmData, v);
    };
    EXPECT_TRUE(sb.owns(SoundBlaster::kAdLibFmAddr));
    EXPECT_TRUE(sb.owns(SoundBlaster::kAdLibFmData));

    fm_write(0x04, 0x60);
    fm_write(0x04, 0x80);
    EXPECT_EQ(sb.in(SoundBlaster::kAdLibFmAddr), 0x00);
    fm_write(0x02, 0xFF);
    fm_write(0x04, 0x21);
    Tick(uint64_t(kCpuHz * 200e-6));
    EXPECT_EQ(sb.in(SoundBlaster::kAdLibFmAddr), 0xC0);

    // Same chip as base+8h/9h: a register written via 0x388 reads back through the card's block.
    fm_write(0x04, 0x80);
    fm_write(0x20, 0x0A);
    EXPECT_EQ(sb.fm.reg(0x20), 0x0A);
}

// 0x38Ah/0x38Bh are the AdLib Gold/PAS bank-1 pair; an SB16 doesn't decode them (bank 1 is base+2h/3h).
TEST_F(SoundBlasterTest, TheCardDoesNotClaimThe38ABankOnePair) {
    EXPECT_FALSE(sb.owns(0x38A));
    EXPECT_FALSE(sb.owns(0x38B));
}

// Bank 1 at base+2h/3h is inert until the OPL3 NEW bit is set.
TEST_F(SoundBlasterTest, FmBankOneReachesTheSecondRegisterBankOnlyInOpl3Mode) {
    sb.out(kBase + 0x02, 0x20);  // bank 1, register 20h
    sb.out(kBase + 0x03, 0x01);
    EXPECT_EQ(sb.fm.reg(0x120), 0x00) << "bank 1 is inert while NEW is clear";
    EXPECT_FALSE(sb.fm.opl3_mode());

    sb.out(kBase + 0x02, 0x05);  // 105h: NEW
    sb.out(kBase + 0x03, 0x01);
    EXPECT_TRUE(sb.fm.opl3_mode());
    sb.out(kBase + 0x02, 0x20);
    sb.out(kBase + 0x03, 0x01);
    EXPECT_EQ(sb.fm.reg(0x120), 0x01);
}

// FM and digitized playback are independent: tick() steps the OPL3 before the DSP path's early-out.
TEST_F(SoundBlasterTest, FmKeepsPlayingWhileTheDigitizedPathIsIdle) {
    const uint16_t kFmAddr = kBase + 0x08, kFmData = kBase + 0x09;
    auto fm_write = [&](uint8_t reg, uint8_t v) {
        sb.out(kFmAddr, reg);
        sb.out(kFmData, v);
    };
    fm_write(0x20, 0x01); fm_write(0x23, 0x01);  // MULT=1 on both operators
    fm_write(0x40, 0x10); fm_write(0x43, 0x00);  // modest modulator TL, loud carrier
    fm_write(0x60, 0xF0); fm_write(0x63, 0xF0);  // fast attack
    fm_write(0x80, 0x00); fm_write(0x83, 0x00);
    fm_write(0xC0, 0x30);                        // both outputs on
    fm_write(0xA0, 0x98); fm_write(0xB0, 0x2E);  // key on, mid octave

    ASSERT_FALSE(sb.playing()) << "no digitized transfer programmed";
    Tick(uint64_t(kCpuHz * 0.01));
    auto fm_samples = sb.fm.drain_samples();
    EXPECT_FALSE(fm_samples.empty()) << "FM ran with the DSP idle";
    bool any_nonzero = false;
    for (const auto &f : fm_samples) {
        if (f.left != 0 || f.right != 0) { any_nonzero = true; break; }
    }
    EXPECT_TRUE(any_nonzero);
    EXPECT_TRUE(sb.drain_samples().empty()) << "no digitized output was programmed";
}

// --- DSP 4.xx programmed transfers --------------------------------------

TEST_F(SoundBlasterTest, SixteenBitSignedStereoAutoInitUsesChannelFiveAndTheSixteenBitIrq) {
    // Allegro's SB16 path: 41h with the rate, then B6h (16-bit, auto-init, FIFO on) with mode 30h.
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});      // 44100 Hz
    Cmd({0xB6, 0x30, 0x07, 0x00});  // 8 words per block = 4 frames of 16-bit stereo
    EXPECT_TRUE(sb.transfer_is_16bit());
    EXPECT_TRUE(sb.transfer_is_autoinit());
    EXPECT_TRUE(sb.stereo());
    EXPECT_TRUE(sb.sixteen_bit());
    EXPECT_EQ(sb.transfer_dma_channel(), 5);

    // 4 frames of 16-bit stereo = 16 bytes, little-endian signed.
    std::vector<uint8_t> pcm = {
        0x00, 0x00, 0x00, 0x00,  // silence L, silence R
        0x00, 0x40, 0x00, 0xC0,  // +0x4000 L, -0x4000 R
        0xFF, 0x7F, 0x00, 0x80,  // +32767 L, -32768 R
        0x34, 0x12, 0x78, 0x56,
    };
    std::size_t offset = 0, moved = 0;
    while (moved < 16) {
        ASSERT_TRUE(RunUntilTransfer(64));
        EXPECT_EQ(sb.transfer_length() % 4, 0u) << "a burst must not split a stereo frame";
        moved += ServeBurst(pcm, offset);
    }

    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), 4u);
    EXPECT_EQ(samples[0].left, 0);
    EXPECT_EQ(samples[1].left, 0x4000);
    EXPECT_EQ(samples[1].right, int16_t(-0x4000));
    EXPECT_EQ(samples[2].left, 32767);
    EXPECT_EQ(samples[2].right, -32768);
    EXPECT_EQ(samples[3].left, 0x1234);
    EXPECT_EQ(samples[3].right, 0x5678);

    // A 16-bit transfer raises the 16-bit source, which base+Eh must not acknowledge (SBPG 2-5).
    ASSERT_TRUE(sb.irq_pending());
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x02);
    sb.in(kReadStatus);
    EXPECT_TRUE(sb.irq_pending());
    sb.in(kAck16);
    EXPECT_FALSE(sb.irq_pending());
}

// Bxh/Cxh block length counts DMA transfer cycles (bytes on 8-bit, words on 16-bit), not frames,
// so a stereo block spans half as many frames. Reading frames doubled DOOM 1.2's effects (begin_dma).
TEST_F(SoundBlasterTest, StereoBlockLengthCountsDmaUnitsNotFrames) {
    ResetDsp();
    Cmd({0x41, 0x2B, 0x11});  // 11025 Hz, exactly as DOOM 1.2 programs it
    // C6h mode 20h length 00FFh is DOOM's command: 256 bytes = 128 frames.
    Cmd({0xC6, 0x20, 0xFF, 0x00});
    ASSERT_TRUE(sb.stereo());
    ASSERT_FALSE(sb.sixteen_bit());
    ASSERT_TRUE(sb.transfer_is_autoinit());

    std::vector<uint8_t> pcm(256, 0x80);
    std::size_t offset = 0, moved = 0;
    while (!sb.irq8_pending()) {
        ASSERT_TRUE(RunUntilTransfer(4096));
        moved += ServeBurst(pcm, offset);
        ASSERT_LE(moved, 256u) << "block ran past the 256 bytes DOOM programmed";
    }
    EXPECT_EQ(moved, 256u) << "one block must consume 256 bytes, not 512";
    EXPECT_EQ(sb.drain_samples().size(), 128u) << "256 bytes of 8-bit stereo is 128 frames";

    // Mono: length is bytes, one per frame.
    ResetDsp();
    Cmd({0x41, 0x2B, 0x11});
    Cmd({0xC0, 0x00, 0xFF, 0x00});  // 8-bit mono single-cycle, 256 bytes
    std::size_t mono_moved = 0;
    offset = 0;
    while (sb.playing()) {
        ASSERT_TRUE(RunUntilTransfer(4096));
        mono_moved += ServeBurst(pcm, offset);
        ASSERT_LE(mono_moved, 256u);
    }
    EXPECT_EQ(mono_moved, 256u);
    EXPECT_EQ(sb.drain_samples().size(), 256u) << "256 bytes of 8-bit mono is 256 frames";
}

TEST_F(SoundBlasterTest, EightBitUnsignedStereoSingleCycleViaCxCommand) {
    ResetDsp();
    Cmd({0x41, 0x2B, 0x11});        // 11025 Hz exactly
    EXPECT_EQ(sb.sample_rate_hz(), 11025u);
    Cmd({0xC0, 0x20, 0x03, 0x00});  // 8-bit single-cycle, mode 20h = stereo unsigned; 4 bytes = 2 frames
    EXPECT_FALSE(sb.transfer_is_16bit());
    EXPECT_FALSE(sb.transfer_is_autoinit());
    EXPECT_TRUE(sb.stereo());
    EXPECT_EQ(sb.transfer_dma_channel(), 1);

    std::vector<uint8_t> pcm = {0xFF, 0x00, 0x80, 0x80};  // 2 frames
    std::size_t offset = 0, moved = 0;
    while (sb.playing()) {
        ASSERT_TRUE(RunUntilTransfer());
        moved += ServeBurst(pcm, offset);
    }
    EXPECT_EQ(moved, 4u);
    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].left, 0x7F00);
    EXPECT_EQ(samples[0].right, -32768);
    EXPECT_EQ(samples[1].left, 0);
    EXPECT_EQ(samples[1].right, 0);
    // 8-bit width raises the legacy 8-bit source.
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);
}

TEST_F(SoundBlasterTest, SixteenBitUnsignedDataIsCenteredAtEightThousandHex) {
    // SBPG Bxh: minimum amplitude is 0000h signed, 8000h unsigned.
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});
    Cmd({0xB0, 0x00, 0x01, 0x00});  // 16-bit single-cycle, mode 00h = mono unsigned
    EXPECT_FALSE(sb.stereo());

    std::vector<uint8_t> pcm = {0x00, 0x80, 0x00, 0x00};  // 8000h then 0000h
    std::size_t offset = 0;
    while (sb.playing()) {
        ASSERT_TRUE(RunUntilTransfer(64));
        ServeBurst(pcm, offset);
    }
    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].left, 0);             // 8000h unsigned == silence
    EXPECT_EQ(samples[1].left, -32768);        // 0000h unsigned == full negative
}

TEST_F(SoundBlasterTest, RecordingIsPacedAndDeliversDigitalSilence) {
    // Nothing is on line/mic, so the card digitizes silence (80h unsigned). The interrupt must still fire.
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x24, 0x03, 0x00});  // 8-bit single-cycle input, 4 bytes
    EXPECT_TRUE(sb.transfer_is_input());
    ASSERT_TRUE(RunUntilTransfer());
    const uint8_t *buf = sb.transfer_buffer();
    for (std::size_t i = 0; i < sb.transfer_length(); ++i) EXPECT_EQ(buf[i], 0x80);
    std::vector<uint8_t> unused;
    std::size_t offset = 0;
    while (sb.playing()) {
        ASSERT_TRUE(RunUntilTransfer());
        ServeBurst(unused, offset);
    }
    EXPECT_TRUE(sb.irq_pending());
    // Recording produces no DAC output at all.
    EXPECT_TRUE(sb.drain_samples().empty());
}

TEST_F(SoundBlasterTest, SilencePeriodEmitsSilentSamplesThenInterrupts) {
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x80, 0x0F, 0x00});  // pause the DAC for 16 sampling periods
    EXPECT_TRUE(sb.playing());
    for (int i = 0; i < 100000 && sb.playing(); ++i) Tick(512);
    EXPECT_FALSE(sb.playing());
    auto samples = sb.drain_samples();
    EXPECT_EQ(samples.size(), 16u);
    for (const auto &s : samples) {
        EXPECT_EQ(s.left, 0);
        EXPECT_EQ(s.right, 0);
    }
    EXPECT_TRUE(sb.irq_pending());
    // No DMA is involved in a silence period.
    EXPECT_FALSE(sb.transfer_ready());
}

TEST_F(SoundBlasterTest, DirectModeOutputLatchesOneSamplePerCommand) {
    ResetDsp();
    Cmd({0x10, 0xFF});
    Cmd({0x10, 0x80});
    Cmd({0x10, 0x00});
    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), 3u);
    EXPECT_EQ(samples[0].left, 0x7F00);
    EXPECT_EQ(samples[1].left, 0);   // 80h: unsigned silence, per SBPG
    EXPECT_EQ(samples[2].left, -32768);
    // Direct mode starts no DMA; the application paces it.
    EXPECT_FALSE(sb.transfer_ready());
}

TEST_F(SoundBlasterTest, DirectModeAdcReturnsTheSilenceMidpoint) {
    // 20h (direct-mode 8-bit ADC) returns the unsigned silence midpoint, as DMA recording does.
    ResetDsp();
    Cmd({0x20});
    EXPECT_EQ(Read(), 0x80);
    EXPECT_FALSE(sb.transfer_ready()) << "direct mode starts no DMA at all";
}

TEST_F(SoundBlasterTest, DmaIdentificationEvolvesItsStateAndArmsAOneByteTransfer) {
    // E2h is undocumented by Creative; the only description is DOSBox's sblaster.cpp (secondary, see
    // begin_dma). valadd/valxor start at 0xAA/0x96 after DSP reset.
    ResetDsp();
    Cmd({0xE2, 0x12});
    const uint8_t expect1 = uint8_t(0xAA + (0x12 ^ 0x96));
    ASSERT_TRUE(sb.transfer_ready());
    EXPECT_EQ(sb.transfer_length(), 1u);
    EXPECT_TRUE(sb.transfer_is_input());
    EXPECT_FALSE(sb.transfer_is_16bit());
    EXPECT_EQ(sb.transfer_dma_channel(), 1);  // the 8-bit channel
    EXPECT_EQ(sb.transfer_buffer()[0], expect1);
    EXPECT_FALSE(sb.irq_pending()) << "a one-byte identification write is not a DSP block";

    sb.finish_transfer(1);
    EXPECT_FALSE(sb.transfer_ready());
    EXPECT_FALSE(sb.playing());
    EXPECT_FALSE(sb.irq_pending());

    // A second E2h continues from the first's state.
    const uint8_t valxor2 = uint8_t((0x96 >> 2) | (0x96 << 6));
    Cmd({0xE2, 0x34});
    const uint8_t expect2 = uint8_t(expect1 + (0x34 ^ valxor2));
    EXPECT_EQ(sb.transfer_buffer()[0], expect2);
}

TEST_F(SoundBlasterTest, HighSpeedCommandsAreIgnoredOnDspFourPointX) {
    // SBPG chapter 6: 90h/91h/98h/99h exist on DSP 2.01+ and 3.xx only. Allegro issues them only below 4.00,
    // so a 4.05 card ignores them.
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x48, 0x0F, 0x00});
    Cmd({0x90});
    EXPECT_FALSE(sb.playing());
    EXPECT_FALSE(RunUntilTransfer(512, 4000));
    // ...and the command stream is still in sync afterward.
    Cmd({0xE1});
    EXPECT_EQ(Read(), 4);
    EXPECT_EQ(Read(), 5);
}

TEST_F(SoundBlasterTest, AdpcmCommandsDoNotDesyncTheCommandStream) {
    // A driver probing 75h must not desync the command stream (its length bytes would parse as commands).
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});  // 8000 Hz
    Cmd({0x75, 0xFF, 0x01});
    EXPECT_TRUE(sb.playing());
    Cmd({0xE1});
    EXPECT_EQ(Read(), 4);
    EXPECT_EQ(Read(), 5);
}

// --- ADPCM output -------------------------------------------------------
// 8-bit mono ADPCM per SBPG Table 3-1. The step tables have no primary source (soundblaster.cpp).

TEST_F(SoundBlasterTest, AdpcmFourBitDecodesTwoSamplesPerCompressedByte) {
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});        // 8000 Hz
    Cmd({0x75, 0x02, 0x00});        // 4-bit with reference, 3 compressed bytes
    // Byte 0 is the reference (SBPG 3-7): it seeds the predictor at 80h and yields no sample.
    // Codes 1 and 2 at step size 0 add +1 then +2.
    std::vector<uint8_t> data{0x80, 0x12, 0x00};
    std::size_t offset = 0;
    std::vector<SoundBlaster::Sample> samples;
    for (int i = 0; i < 64 && !sb.irq8_pending(); ++i) {
        if (!RunUntilTransfer()) break;
        ServeBurst(data, offset);
        for (const auto &s : sb.drain_samples()) samples.push_back(s);
    }
    ASSERT_EQ(samples.size(), 4u) << "two code bytes, two samples each";
    EXPECT_EQ(samples[0].left, int16_t((0x81 - 128) * 256));
    EXPECT_EQ(samples[1].left, int16_t((0x83 - 128) * 256));
    EXPECT_EQ(samples[0].left, samples[0].right) << "ADPCM is mono, duplicated to both channels";
}

TEST_F(SoundBlasterTest, AdpcmThreeAndTwoBitPackMoreSamplesPerByte) {
    // 3-bit "2.6-bit" mode packs three samples per byte, 2-bit four, 4-bit two.
    for (auto [cmd, per_byte] : {std::pair<uint8_t, std::size_t>{0x77, 3},
                                 std::pair<uint8_t, std::size_t>{0x17, 4}}) {
        ResetDsp();
        Cmd({0x41, 0x1F, 0x40});
        Cmd({cmd, 0x02, 0x00});     // reference byte plus two code bytes
        std::vector<uint8_t> data{0x80, 0xB1, 0x4E};
        std::size_t offset = 0;
        std::size_t decoded = 0;
        for (int i = 0; i < 64 && !sb.irq8_pending(); ++i) {
            if (!RunUntilTransfer()) break;
            ServeBurst(data, offset);
            decoded += sb.drain_samples().size();
        }
        EXPECT_EQ(decoded, per_byte * 2) << "command " << std::hex << int(cmd);
    }
}

TEST_F(SoundBlasterTest, AdpcmBlockCounterCountsCompressedBytesNotSamples) {
    // The length counts compressed bytes, so the interrupt lands after that many bytes move.
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});
    Cmd({0x75, 0x03, 0x00});        // 4 compressed bytes
    std::vector<uint8_t> data{0x80, 0x11, 0x22, 0x33};
    std::size_t offset = 0;
    std::size_t moved = 0;
    for (int i = 0; i < 64 && !sb.irq8_pending(); ++i) {
        if (!RunUntilTransfer()) break;
        moved += ServeBurst(data, offset);
    }
    EXPECT_TRUE(sb.irq8_pending()) << "no block interrupt after the programmed byte count";
    EXPECT_EQ(moved, 4u);
    // 1 reference byte + 3 code bytes at 2 samples each.
    EXPECT_EQ(sb.drain_samples().size(), 6u);
}

TEST_F(SoundBlasterTest, AdpcmPlaybackIsPacedAtTheRealSampleRate) {
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});        // 8000 Hz
    Cmd({0x48, 0x3F, 0x00});        // 64 compressed bytes per block
    Cmd({0x7D});                    // auto-init 4-bit, length from 48h
    std::vector<uint8_t> data(64, 0x11);
    data[0] = 0x80;
    std::size_t offset = 0;
    const uint64_t start = cycles_;
    std::size_t frames = 0;
    while (frames < 64 && cycles_ - start < uint64_t(kCpuHz)) {
        if (!RunUntilTransfer()) break;
        ServeBurst(data, offset);
        frames = sb.drain_samples().size() + frames;
    }
    // 64 samples at 8000 Hz is 8 ms; the card must take at least that long.
    const double elapsed_s = double(cycles_ - start) / kCpuHz;
    EXPECT_GE(elapsed_s, 0.0075) << "ADPCM playback ran faster than its own sample rate";
}

TEST_F(SoundBlasterTest, AdpcmAutoInitRepeatsUntilTheExitCommand) {
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});
    Cmd({0x48, 0x03, 0x00});        // 4 compressed bytes per block
    Cmd({0x7D});                    // auto-init 4-bit ADPCM
    std::vector<uint8_t> data{0x80, 0x11, 0x22, 0x33};
    std::size_t offset = 0;
    int blocks = 0;
    for (int i = 0; i < 400 && blocks < 3; ++i) {
        if (!RunUntilTransfer()) break;
        ServeBurst(data, offset);
        if (sb.irq8_pending()) { ++blocks; sb.in(kReadStatus); }
    }
    EXPECT_EQ(blocks, 3) << "auto-init must keep reloading its block";
    Cmd({0xDA});                    // exit auto-init at the end of this block
    for (int i = 0; i < 400 && sb.playing(); ++i) {
        if (!RunUntilTransfer()) break;
        ServeBurst(data, offset);
    }
    EXPECT_FALSE(sb.playing());
}

TEST_F(SoundBlasterTest, DmaIdentificationStaysArmedUntilTheChannelIsServiced) {
    // A driver finds its DMA channel with E2h, so the request stays asserted until the channel is
    // unmasked and serviced; a masked channel must not consume the byte.
    ResetDsp();
    Cmd({0xE2, 0x00});
    ASSERT_TRUE(sb.transfer_ready());
    EXPECT_TRUE(sb.transfer_is_input()) << "identification is a write to memory";
    EXPECT_EQ(sb.transfer_length(), 1u);
    const uint8_t expected = sb.transfer_buffer()[0];
    sb.finish_transfer(0);          // masked channel: nothing moved
    EXPECT_TRUE(sb.transfer_ready()) << "the byte must not be dropped";
    EXPECT_EQ(sb.transfer_buffer()[0], expected);
    sb.finish_transfer(1);          // unmasked: the byte goes out
    EXPECT_FALSE(sb.transfer_ready());
    EXPECT_FALSE(sb.irq8_pending()) << "identification raises no interrupt";
    // 0AAh + (00h ^ 96h) = 40h on the first call.
    EXPECT_EQ(expected, 0x40);
}

TEST_F(SoundBlasterTest, DspResetStopsAnActiveTransferAndClearsPendingInterrupts) {
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x14, 0xFF, 0x00});
    ASSERT_TRUE(RunUntilTransfer());
    Cmd({0xF2});
    ASSERT_TRUE(sb.irq_pending());
    ResetDsp();
    EXPECT_FALSE(sb.playing());
    EXPECT_FALSE(sb.transfer_ready());
    EXPECT_FALSE(sb.irq_pending());
}

TEST_F(SoundBlasterTest, AssertingResetAloneHaltsPlaybackWithoutWaitingForTheZero) {
    // Reset is held for as long as the line is high: a transfer stops when the 1 is written, not at
    // the 1->0 edge. Mixer volumes and the test register survive.
    ResetDsp();
    Cmd({0x40, 166});
    Cmd({0x14, 0xFF, 0x00});
    ASSERT_TRUE(RunUntilTransfer());
    MixerWrite(0x30, 0x55);
    Cmd({0xE4, 0x3C});  // test register

    sb.out(kReset, 1);  // assert only -- no 0 follows yet
    EXPECT_FALSE(sb.playing());
    EXPECT_FALSE(sb.transfer_ready());
    EXPECT_FALSE(RunUntilTransfer(512, 4000)) << "a held-in-reset card must not keep requesting bytes";
    EXPECT_EQ(MixerRead(0x30), 0x55);

    // The 0 still completes the handshake.
    sb.out(kReset, 0);
    ASSERT_TRUE(sb.in(kReadStatus) & 0x80);
    EXPECT_EQ(sb.in(kReadData), 0xAA);
    Cmd({0xE8});
    EXPECT_EQ(Read(), 0x3C);  // test register survives a DSP reset
}

// --- CT1745 mixer -------------------------------------------------------

TEST_F(SoundBlasterTest, MixerIrqAndDmaSelectionDefaultToTheStandardSb16Jumpers) {
    // SBPG 2-6/2-7: mixer 80h bit 1 = IRQ5, 81h bit 1 = DMA1, bit 5 = DMA5 (SET BLASTER=A220 I5 D1 H5).
    EXPECT_EQ(MixerRead(0x80), 0x02);
    EXPECT_EQ(MixerRead(0x81), 0x22);
    EXPECT_EQ(sb.irq_line(), 5);
    EXPECT_EQ(sb.dma_channel_8bit(), 1);
    EXPECT_EQ(sb.dma_channel_16bit(), 5);

    // Software-configurable on DSP 4.xx; the chipset follows it.
    MixerWrite(0x80, 0x04);  // IRQ7
    EXPECT_EQ(sb.irq_line(), 7);
    MixerWrite(0x81, 0x01 | 0x80);  // DMA0 + DMA7
    EXPECT_EQ(sb.dma_channel_8bit(), 0);
    EXPECT_EQ(sb.dma_channel_16bit(), 7);
    MixerWrite(0x80, 0x00);
    EXPECT_EQ(sb.irq_line(), -1) << "no line selected: the card drives no interrupt";
}

TEST_F(SoundBlasterTest, MixerDefaultsMatchTheDocumentedCt1745PowerOnValues) {
    // SBPG chapter 4 defaults: master/voice/MIDI 24 (-14 dB), CD/line/mic 0, switches closed, treble/bass 8.
    for (uint8_t i = 0x30; i <= 0x35; ++i) EXPECT_EQ(MixerRead(i), 24 << 3) << std::hex << int(i);
    for (uint8_t i = 0x36; i <= 0x3B; ++i) EXPECT_EQ(MixerRead(i), 0x00) << std::hex << int(i);
    EXPECT_EQ(MixerRead(0x3C), 0x1F);
    EXPECT_EQ(MixerRead(0x3D), 0x15);
    EXPECT_EQ(MixerRead(0x3E), 0x0B);
    for (uint8_t i = 0x44; i <= 0x47; ++i) EXPECT_EQ(MixerRead(i), 8 << 4) << std::hex << int(i);
    // CT1345-compatibility registers read 12 per channel (SBPG default) and alias 0x30-0x35.
    EXPECT_EQ(MixerRead(0x22), 0xCC);
    EXPECT_EQ(MixerRead(0x04), 0xCC);
    EXPECT_EQ(MixerRead(0x28), 0x00);
}

TEST_F(SoundBlasterTest, CompatibilityVolumeRegistersAliasTheNewOnes) {
    // 4 bits at 4 dB steps map onto 5 bits at 2 dB steps: n maps to 2n+1.
    MixerWrite(0x22, 0xF0);  // master: left full, right minimum
    EXPECT_EQ(MixerRead(0x30), 31 << 3);
    EXPECT_EQ(MixerRead(0x31), 1 << 3);
    EXPECT_EQ(MixerRead(0x22), 0xF0);  // and reads back as written
    MixerWrite(0x04, 0x0F);  // voice: left minimum, right full
    EXPECT_EQ(MixerRead(0x32), 1 << 3);
    EXPECT_EQ(MixerRead(0x33), 31 << 3);
}

TEST_F(SoundBlasterTest, OutputGainCombinesMasterAndVoiceAttenuators) {
    // Both attenuators are analog and multiply. Exposed to the front end, not folded into drain_samples().
    MixerWrite(0x30, 31 << 3);
    MixerWrite(0x31, 31 << 3);
    MixerWrite(0x32, 31 << 3);
    MixerWrite(0x33, 31 << 3);
    EXPECT_NEAR(sb.output_gain_left(), 1.0f, 1e-4f);   // 0 dB * 0 dB
    EXPECT_NEAR(sb.output_gain_right(), 1.0f, 1e-4f);
    MixerWrite(0x32, 26 << 3);  // voice -10 dB
    EXPECT_NEAR(sb.output_gain_left(), 0.31623f, 1e-3f);
    EXPECT_NEAR(sb.output_gain_right(), 1.0f, 1e-4f);
}

TEST_F(SoundBlasterTest, OutputGainRegisterBoostsBothTheDigitizedAndFmLegs) {
    // Mixer 41h/42h (Output Gain) apply to both Voice (output_gain_*) and FM (fm_gain_*) legs:
    // SBPG chapter 4, 2 bits, 0-18 dB in 6 dB steps.
    MixerWrite(0x30, 31 << 3); MixerWrite(0x31, 31 << 3);  // master 0 dB
    MixerWrite(0x32, 31 << 3); MixerWrite(0x33, 31 << 3);  // voice 0 dB
    MixerWrite(0x34, 31 << 3); MixerWrite(0x35, 31 << 3);  // MIDI/FM 0 dB
    ASSERT_NEAR(sb.output_gain_left(), 1.0f, 1e-4f);
    ASSERT_NEAR(sb.fm_gain_left(), 1.0f, 1e-4f);

    MixerWrite(0x41, 0x01 << 6);  // Output Gain Left level 1 => +6 dB
    EXPECT_NEAR(sb.output_gain_left(), 1.99526f, 1e-3f);
    EXPECT_NEAR(sb.fm_gain_left(), 1.99526f, 1e-3f);
    EXPECT_NEAR(sb.output_gain_right(), 1.0f, 1e-4f);  // right leg untouched
    EXPECT_NEAR(sb.fm_gain_right(), 1.0f, 1e-4f);

    MixerWrite(0x42, 0x03 << 6);  // Output Gain Right level 3 => +18 dB
    EXPECT_NEAR(sb.output_gain_right(), 7.94328f, 1e-3f);
    EXPECT_NEAR(sb.fm_gain_right(), 7.94328f, 1e-3f);
}

TEST_F(SoundBlasterTest, MixerResetRegisterRestoresDefaultsButADspResetDoesNot) {
    // 22h is written first because it aliases 30h/31h.
    MixerWrite(0x22, 0x00);
    MixerWrite(0x30, 0x00);
    ASSERT_EQ(MixerRead(0x30), 0x00);
    // The mixer is a separate chip; a DSP reset leaves its volumes alone.
    ResetDsp();
    EXPECT_EQ(MixerRead(0x30), 0x00);
    // Register 00h: any write resets the mixer.
    MixerWrite(0x00, 0x00);
    EXPECT_EQ(MixerRead(0x30), 24 << 3);
    EXPECT_EQ(MixerRead(0x80), 0x02);
}

TEST_F(SoundBlasterTest, InterruptStatusRegisterIsReadOnly) {
    ResetDsp();
    Cmd({0xF2});
    MixerWrite(0x82, 0x00);  // software cannot clear it this way
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);
    EXPECT_TRUE(sb.irq_pending());
}

TEST_F(SoundBlasterTest, MixerAddressRegisterReadsBackTheSelectedIndex) {
    sb.out(kMixerAddr, 0x30);
    EXPECT_EQ(sb.in(kMixerAddr), 0x30);
}

// --- sample log bookkeeping ---------------------------------------------

TEST_F(SoundBlasterTest, DrainSamplesReturnsAndClearsTheLog) {
    ResetDsp();
    Cmd({0x10, 0x80});
    EXPECT_EQ(sb.drain_samples().size(), 1u);
    EXPECT_TRUE(sb.drain_samples().empty());
}

TEST_F(SoundBlasterTest, SampleLogOverflowDropsTheOldestSampleNotTheNewest) {
    // Real hardware has no limit; this bounds memory if nothing drains (like PcSpeaker::kMaxEdges).
    ResetDsp();
    constexpr int kMaxSamples = 1 << 16;
    for (int i = 0; i < kMaxSamples + 5; ++i) {
        sb.out(kWriteCmd, 0x10);
        sb.out(kWriteCmd, uint8_t(i & 0xFF));
    }
    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), std::size_t(kMaxSamples));
    // The oldest survivor was written at i == 5.
    EXPECT_EQ(samples.front().left, int16_t((5 - 128) * 256));
    EXPECT_EQ(samples.back().left, int16_t((((kMaxSamples + 4) & 0xFF) - 128) * 256));
}

}  // namespace
