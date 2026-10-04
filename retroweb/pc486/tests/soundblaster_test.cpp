// GoogleTest suite for the Sound Blaster 16: port decode across the whole
// 0x220-0x22F block, the DSP reset handshake and its 0AAh acknowledge byte,
// the identification commands a driver uses to find the card, the legacy
// 8-bit and the DSP 4.xx Cxh/Bxh digitized-output command sets, real-time
// pacing of DMA-driven playback, the transfer_ready()/transfer_buffer()/
// finish_transfer() handoff chipset.cpp uses to move the bytes, per-source
// interrupt status and its two separate acknowledge ports, and the CT1745
// mixer's defaults and compatibility aliases.
//
// Register/protocol details are checked against Creative's own "Sound
// Blaster Series Hardware Programming Guide" (cited as SBPG, with the
// chapter/page it comes from) rather than against another emulator.

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

    // SBPG 2-2: write a 1 to the reset port, wait, write a 0, then poll the
    // Read-Buffer Status port and read 0AAh from the Read Data port.
    void ResetDsp() {
        sb.out(kReset, 1);
        sb.out(kReset, 0);
        ASSERT_TRUE(sb.in(kReadStatus) & 0x80) << "no acknowledge byte waiting after reset";
        ASSERT_EQ(sb.in(kReadData), 0xAA);
    }

    // SBPG 2-4: poll the Write-Buffer Status port for bit 7 clear, then write
    // the command or data byte. Every DSP write in these tests goes through
    // here so the ready bit is genuinely exercised on the real path.
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

    // Advances time in small steps until the card asserts its DMA request.
    // Returns false if it never does within the budget.
    bool RunUntilTransfer(uint64_t step = 512, int max_steps = 4000000) {
        for (int i = 0; i < max_steps && !sb.transfer_ready(); ++i) Tick(step);
        return sb.transfer_ready();
    }

    // Plays the chipset's part for one burst: fills the card's buffer from
    // `data` (playback) or drops what it produced (record), then completes
    // the transfer. Returns how many bytes moved.
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
    // A real card decodes base+0h..base+13h (SBPG Appendix A, Table A-15),
    // all twenty ports -- base+10h-13h is the card's own CD-ROM interface,
    // not just the DSP's four ports.
    for (uint16_t p = 0x220; p <= 0x233; ++p) EXPECT_TRUE(sb.owns(p)) << std::hex << p;
    EXPECT_FALSE(sb.owns(0x21F));
    EXPECT_FALSE(sb.owns(0x234));
    // ...plus the AdLib card's own FM pair, which every Sound Blaster answers.
    EXPECT_TRUE(sb.owns(0x388));
    EXPECT_TRUE(sb.owns(0x389));
}

TEST_F(SoundBlasterTest, CdRomInterfacePortsAreOpenBusWithNoDriveAttached) {
    // This machine's CD-ROM is on the IDE/ATAPI channel instead -- the
    // card's own proprietary CD-ROM interface at base+10h-13h is present
    // (decoded) but has no drive behind it: reads float high, writes vanish.
    for (uint16_t p = 0x230; p <= 0x233; ++p) {
        EXPECT_EQ(sb.in(p), 0xFF) << std::hex << p;
        sb.out(p, 0x00);           // must not disturb anything else
        EXPECT_EQ(sb.in(p), 0xFF) << std::hex << p;
    }
}

TEST_F(SoundBlasterTest, CdGainFollowsMixerVolumeAndIsGatedByTheOutputSwitch) {
    // Mixer 36h/37h (CD volume L/R) into the same five-bit attenuator as
    // Voice/Line, gated by 3Ch's CD.L/CD.R output switches (bits 2/1) --
    // same CT1745 register map atapi_cdrom.h's CD-DA output is meant to
    // reach once it exists (PC486_REVIEW.md's "Open on the SB16" note).
    // Default volume (0 -> -62 dB) is near-silent but the switches default
    // closed (connected), so there is something to gate in the first place.
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
    // SBPG 2-2 is explicit that the sequence is a 1 followed by a 0; a card
    // left parked in reset posts nothing, which is exactly what a driver
    // that mis-sequences the handshake sees.
    sb.out(kReset, 1);
    EXPECT_EQ(sb.in(kReadStatus) & 0x80, 0x00);
    sb.out(kReset, 0);
    ASSERT_TRUE(sb.in(kReadStatus) & 0x80);
    EXPECT_EQ(sb.in(kReadData), 0xAA);
    // FIFO drained: the status bit drops again.
    EXPECT_EQ(sb.in(kReadStatus) & 0x80, 0x00);
}

TEST_F(SoundBlasterTest, DrainedReadFifoKeepsReturningTheLastByte) {
    ResetDsp();
    // Real hardware re-presents the last byte read rather than floating.
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
    // The DOS driver BOOM links (Allegro 3.x sb.c) builds
    // (major << 8) | minor and takes its SB16 path only at >= 0x400 --
    // report less and it silently drops to the SB-Pro path. See
    // soundblaster.h.
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
    // Period drivers read this to tell a genuine Creative card from a clone.
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
    // F2h/F3h are how a driver's IRQ auto-detection finds out which line the
    // card is actually programmed to.
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
    // SBPG chapter 3: only the high byte of
    // 65536 - 256000000/(channels*rate) is programmed, which is what a
    // driver writes as 256 - 1000000/rate. 166 is the byte a driver asking
    // for 11025 Hz writes, and the card really clocks 1000000/90 Hz.
    Cmd({0x40, 166});
    EXPECT_EQ(sb.sample_rate_hz(), 1000000u / 90u);
    // 41h carries the true rate in Hz, HIGH byte first -- the opposite byte
    // order from every length parameter on the card (SBPG 3-26).
    // 22050 Hz is 5622h: high byte 56h first. Swapping the two bytes would
    // give 2256h -- 8790 Hz -- so this ordering is worth pinning down.
    Cmd({0x41, 0x56, 0x22});
    EXPECT_EQ(sb.sample_rate_hz(), 22050u);
    Cmd({0x41, 0xAC, 0x44});
    EXPECT_EQ(sb.sample_rate_hz(), 44100u);   // SBPG's own worked example
}

TEST_F(SoundBlasterTest, SamplingRateClampsToWhatTheDspCanClockAtAll) {
    // The rate registers hold anything the DSP can clock: 4000 Hz is the
    // lowest figure anywhere in SBPG Tables 3-2/3-3 (the ADPCM rows) and
    // 44100 the highest. Which range actually applies depends on the format
    // the transfer command selects, so that limit lands when a transfer
    // starts -- see the two tests below. Real hardware clamps rather than
    // rejecting an out-of-range request.
    ResetDsp();
    Cmd({0x41, 0x00, 0x00});  // 0 Hz requested
    EXPECT_EQ(sb.sample_rate_hz(), 4000u);
    Cmd({0x41, 0xFF, 0xFF});  // 65535 Hz requested
    EXPECT_EQ(sb.sample_rate_hz(), 44100u);
    // The 40h time-constant path can ask for far outside the range too: tc 0
    // inverts to ~3906 Hz, tc 255 to 1 MHz.
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
    // Table 3-2's ADPCM rows stop lower than PCM does: 4-bit at 12000 Hz,
    // 3-bit at 13000, 2-bit at 11000, all from 4000.
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

    // 80h is silence for unsigned 8-bit data, 00h and FFh the extremes.
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
    // Timestamps must advance, in order, at the programmed rate.
    EXPECT_LT(samples[0].cpu_cycle, samples[7].cpu_cycle);

    // End of block raises the 8-bit interrupt, reported through mixer 82h.
    EXPECT_TRUE(sb.irq_pending());
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);
    sb.in(kReadStatus);
    EXPECT_FALSE(sb.irq_pending());
}

TEST_F(SoundBlasterTest, PlaybackIsPacedAtTheRealSampleRateAndNeverFaster) {
    // The realism guard: 256 samples at a programmed 11 kHz must take the
    // wall-clock time 256 samples actually take on a real card. CLAUDE.md
    // forbids speeding this up, and getting it wrong is silent -- audio just
    // plays at the wrong pitch.
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
    // PC486_REVIEW.md's DMA-timing item, "cheap middle option": a burst's
    // bytes move in one step at the end of the window they cover, so capping
    // that window to about 1ms bounds how far behind real hardware a byte
    // can be heard -- instead of the whole 256KB internal buffer moving (and
    // being heard) in a single step, which is what a big block did before
    // this cap existed.
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

    // DAh exits "at the end of the current block transfer", not immediately.
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

    // D6h continues *16-bit* transfers; a real card ignores it here.
    Cmd({0xD6});
    EXPECT_FALSE(sb.playing());

    Cmd({0xD4});  // continue 8-bit DMA
    EXPECT_TRUE(sb.playing());
    EXPECT_TRUE(RunUntilTransfer());
}

// --- FM (OPL3) through the card's own ports ------------------------------

// The reason the OPL3 exists: a driver must be able to FIND it through the
// card's port block. This is the canonical AdLib detection sequence run
// against base+8h/9h, the AdLib-compatible alias every period program uses.
// Before the OPL3 was wired up these ports returned 00h and this failed by
// design, so software concluded the machine had no music hardware at all.
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
    // base+0h and base+2h are the same status register, not three of them.
    EXPECT_EQ(sb.in(kBase + 0x00), 0xC0);
    EXPECT_EQ(sb.in(kBase + 0x02), 0xC0);

    fm_write(0x04, 0x60);
    fm_write(0x04, 0x80);
    EXPECT_EQ(sb.in(kFmAddr), 0x00);
}

// 0x388/0x389 is the original AdLib card's own FM pair, and every Sound
// Blaster answers it for compatibility. This is the path that actually
// matters: an AdLib-era music driver writes FM registers here and never
// touches the card's own port block, so a machine that decodes base+0h..3h
// but not 0x388 detects an OPL and then plays nothing at all.
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

    // It is the same chip as base+8h/9h, not a second one: a register written
    // through 0x388 reads back through the card's own block.
    fm_write(0x04, 0x80);
    fm_write(0x20, 0x0A);
    EXPECT_EQ(sb.fm.reg(0x20), 0x0A);
}

// 0x38Ah/0x38Bh are the bank-1 pair an AdLib Gold or PAS puts there. An SB16
// does not decode them -- its OPL3 second bank is at base+2h/3h.
TEST_F(SoundBlasterTest, TheCardDoesNotClaimThe38ABankOnePair) {
    EXPECT_FALSE(sb.owns(0x38A));
    EXPECT_FALSE(sb.owns(0x38B));
}

// Bank 1 lives at base+2h/3h and is inert until the OPL3 NEW bit is set,
// which is how an OPL2-era program and an OPL3-aware one share the ports.
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

// FM and digitized playback are independent: music must keep sounding while
// no sample block is in flight, which is why tick() steps the OPL3 before
// the DSP path's early-out.
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
    // Exactly what Allegro's SB16 path issues: 41h with the rate, then B6h
    // (16-bit, auto-init, FIFO on) with mode 30h (16-bit signed stereo).
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});      // 44100 Hz
    Cmd({0xB6, 0x30, 0x07, 0x00});  // 8 words per block = 4 frames of 16-bit stereo
    EXPECT_TRUE(sb.transfer_is_16bit());
    EXPECT_TRUE(sb.transfer_is_autoinit());
    EXPECT_TRUE(sb.stereo());
    EXPECT_TRUE(sb.sixteen_bit());
    EXPECT_EQ(sb.transfer_dma_channel(), 5);

    // 4 frames of 16-bit stereo = 16 bytes. Little-endian signed pairs.
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

    // A 16-bit transfer raises the 16-bit source, which base+Eh must NOT
    // acknowledge -- that is the whole reason SB16 added base+Fh (SBPG 2-5).
    ASSERT_TRUE(sb.irq_pending());
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x02);
    sb.in(kReadStatus);
    EXPECT_TRUE(sb.irq_pending());
    sb.in(kAck16);
    EXPECT_FALSE(sb.irq_pending());
}

// The DSP's Bxh/Cxh block length counts DMA transfer cycles -- bytes on the
// 8-bit channel, words on the 16-bit one -- NOT audio frames, so a stereo
// block spans half as many frames as the programmed length. Reading it as
// frames made every 8-bit stereo block twice too long, which is what made
// DOOM 1.2's sound effects each play twice (see begin_dma's comment).
TEST_F(SoundBlasterTest, StereoBlockLengthCountsDmaUnitsNotFrames) {
    ResetDsp();
    Cmd({0x41, 0x2B, 0x11});  // 11025 Hz, exactly as DOOM 1.2 programs it
    // C6h mode 20h length 00FFh: DOOM's real command. 256 BYTES = 128 frames.
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

    // Mono is unaffected: the length is already in bytes, one byte per frame.
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
    // 8-bit width, so the legacy 8-bit interrupt source, not the 16-bit one.
    EXPECT_EQ(MixerRead(0x82) & 0x03, 0x01);
}

TEST_F(SoundBlasterTest, SixteenBitUnsignedDataIsCenteredAtEightThousandHex) {
    // SBPG's Bxh page: "For minimum signal amplitude, the signed 16-bit value
    // is 0000h; with unsigned data, the equivalent value is 8000h."
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
    // Nothing is connected to the line/mic inputs, so a real card digitizes
    // silence -- 80h in unsigned 8-bit. The interrupt still has to fire or a
    // recording program waits forever.
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
    // Direct mode starts no DMA at all -- the application paces it itself.
    EXPECT_FALSE(sb.transfer_ready());
}

TEST_F(SoundBlasterTest, DirectModeAdcReturnsTheSilenceMidpoint) {
    // 20h (direct-mode 8-bit ADC): nothing is plugged into the line/mic
    // inputs, so the one byte returned through the read FIFO is the
    // unsigned-8-bit silence midpoint -- same reasoning fill_input_buffer
    // already uses for DMA-driven recording.
    ResetDsp();
    Cmd({0x20});
    EXPECT_EQ(Read(), 0x80);
    EXPECT_FALSE(sb.transfer_ready()) << "direct mode starts no DMA at all";
}

TEST_F(SoundBlasterTest, DmaIdentificationEvolvesItsStateAndArmsAOneByteTransfer) {
    // E2h is undocumented by Creative; the only description anywhere is
    // DOSBox/DOSBox-X's sblaster.cpp -- second-hand corroboration, not a
    // primary source (see begin_dma's own block-counter comment). valadd and
    // valxor start at 0xAA/0x96 out of a DSP reset.
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

    // A second E2h evolves from where the first left off, not from scratch.
    const uint8_t valxor2 = uint8_t((0x96 >> 2) | (0x96 << 6));
    Cmd({0xE2, 0x34});
    const uint8_t expect2 = uint8_t(expect1 + (0x34 ^ valxor2));
    EXPECT_EQ(sb.transfer_buffer()[0], expect2);
}

TEST_F(SoundBlasterTest, HighSpeedCommandsAreIgnoredOnDspFourPointX) {
    // SBPG chapter 6's availability matrix lists 90h/91h/98h/99h for DSP
    // 2.01+ and 3.xx only: DSP 4.xx has no high-speed mode. Allegro issues
    // them only after detecting a version below 4.00, so a card reporting
    // 4.05 must ignore them exactly as real hardware does.
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
    // A driver probing 75h must not be able to knock the command stream out
    // of step: its two length bytes would otherwise be read as commands.
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});  // 8000 Hz
    Cmd({0x75, 0xFF, 0x01});
    EXPECT_TRUE(sb.playing());
    Cmd({0xE1});
    EXPECT_EQ(Read(), 4);
    EXPECT_EQ(Read(), 5);
}

// --- ADPCM output -------------------------------------------------------
// SBPG Table 3-1 lists 8-bit mono ADPCM single-cycle and auto-initialize for
// DSP 4.xx, so these are a real capability of this card. The decoder's step
// tables are the one part with no primary source (see soundblaster.cpp).

TEST_F(SoundBlasterTest, AdpcmFourBitDecodesTwoSamplesPerCompressedByte) {
    ResetDsp();
    Cmd({0x41, 0x1F, 0x40});        // 8000 Hz
    Cmd({0x75, 0x02, 0x00});        // 4-bit with reference, 3 compressed bytes
    // Byte 0 is the reference (SBPG 3-7), so it seeds the predictor at 80h
    // and yields no sample of its own; the two code bytes then give two
    // samples each. Codes 1 and 2 at step size 0 add +1 then +2 to the
    // reference, which is the table walk this asserts. A burst carries only
    // as many bytes as have come due at the sample rate, so the block takes
    // several of them.
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
    // The 3-bit "2.6-bit" mode packs three samples into eight bits and the
    // 2-bit mode four, against the 4-bit mode's two.
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
    // The DSP's length parameter for these commands counts compressed bytes,
    // so the block interrupt lands after that many bytes have moved -- not
    // after that many decoded samples.
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
    // 64 decoded samples at 8000 Hz is 8 ms of audio; the card must have
    // taken at least that much wall-clock time to ask for them.
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
    // E2h's single-byte write is what a driver uses to work out which DMA
    // channel the card is really on, so the request has to stay asserted
    // until that channel is actually unmasked and serviced -- a masked
    // channel moves nothing and must not consume the byte.
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
    // 0AAh + (00h ^ 96h) = 40h on the first call, per the evolving state.
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
    // Real hardware holds the DSP in reset for as long as the line stands
    // high: an in-progress transfer stops the instant the 1 is written, not
    // at the eventual 1->0 edge (which is only needed for the 0AAh
    // handshake). Mixer volumes and the test register are a separate chip
    // and survive untouched.
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

    // The 0 still completes the handshake afterward.
    sb.out(kReset, 0);
    ASSERT_TRUE(sb.in(kReadStatus) & 0x80);
    EXPECT_EQ(sb.in(kReadData), 0xAA);
    Cmd({0xE8});
    EXPECT_EQ(Read(), 0x3C);  // test register survives a DSP reset
}

// --- CT1745 mixer -------------------------------------------------------

TEST_F(SoundBlasterTest, MixerIrqAndDmaSelectionDefaultToTheStandardSb16Jumpers) {
    // SBPG 2-6/2-7: mixer 80h bit 1 = IRQ5, mixer 81h bit 1 = DMA1 and bit
    // 5 = DMA5 -- i.e. `SET BLASTER=A220 I5 D1 H5`.
    EXPECT_EQ(MixerRead(0x80), 0x02);
    EXPECT_EQ(MixerRead(0x81), 0x22);
    EXPECT_EQ(sb.irq_line(), 5);
    EXPECT_EQ(sb.dma_channel_8bit(), 1);
    EXPECT_EQ(sb.dma_channel_16bit(), 5);

    // Software-configurable on DSP 4.xx, and the chipset has to follow it.
    MixerWrite(0x80, 0x04);  // IRQ7
    EXPECT_EQ(sb.irq_line(), 7);
    MixerWrite(0x81, 0x01 | 0x80);  // DMA0 + DMA7
    EXPECT_EQ(sb.dma_channel_8bit(), 0);
    EXPECT_EQ(sb.dma_channel_16bit(), 7);
    MixerWrite(0x80, 0x00);
    EXPECT_EQ(sb.irq_line(), -1) << "no line selected: the card drives no interrupt";
}

TEST_F(SoundBlasterTest, MixerDefaultsMatchTheDocumentedCt1745PowerOnValues) {
    // SBPG chapter 4's per-register defaults: master/voice/MIDI 24 (-14 dB)
    // in the 5-bit left-justified field; CD/line/mic 0; all output switches
    // closed; treble/bass 8 (0 dB).
    for (uint8_t i = 0x30; i <= 0x35; ++i) EXPECT_EQ(MixerRead(i), 24 << 3) << std::hex << int(i);
    for (uint8_t i = 0x36; i <= 0x3B; ++i) EXPECT_EQ(MixerRead(i), 0x00) << std::hex << int(i);
    EXPECT_EQ(MixerRead(0x3C), 0x1F);
    EXPECT_EQ(MixerRead(0x3D), 0x15);
    EXPECT_EQ(MixerRead(0x3E), 0x0B);
    for (uint8_t i = 0x44; i <= 0x47; ++i) EXPECT_EQ(MixerRead(i), 8 << 4) << std::hex << int(i);
    // The CT1345-compatibility registers read back 12 per channel, the value
    // SBPG documents as their default, because they alias 0x30-0x35.
    EXPECT_EQ(MixerRead(0x22), 0xCC);
    EXPECT_EQ(MixerRead(0x04), 0xCC);
    EXPECT_EQ(MixerRead(0x28), 0x00);
}

TEST_F(SoundBlasterTest, CompatibilityVolumeRegistersAliasTheNewOnes) {
    // "They are actually mapped to the new volume control registers" -- 4
    // bits at 4 dB steps onto 5 bits at 2 dB steps, so n maps to 2n+1.
    MixerWrite(0x22, 0xF0);  // master: left full, right minimum
    EXPECT_EQ(MixerRead(0x30), 31 << 3);
    EXPECT_EQ(MixerRead(0x31), 1 << 3);
    EXPECT_EQ(MixerRead(0x22), 0xF0);  // and reads back as written
    MixerWrite(0x04, 0x0F);  // voice: left minimum, right full
    EXPECT_EQ(MixerRead(0x32), 1 << 3);
    EXPECT_EQ(MixerRead(0x33), 31 << 3);
}

TEST_F(SoundBlasterTest, OutputGainCombinesMasterAndVoiceAttenuators) {
    // Both attenuators sit in the analog path, so they multiply. Exposed for
    // the front end rather than folded into drain_samples(), so the samples
    // stay exactly what the program wrote (see soundblaster.h).
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
    // Mixer 41h/42h (Output Gain .L/.R) sit after the mixer in the analog
    // chain, so they apply to both the Voice leg (output_gain_*) and the FM
    // leg (fm_gain_*) -- SBPG chapter 4: 2 bits, 0-3 => 0 dB to 18 dB in 6 dB
    // steps.
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
    // 22h is written first because it aliases 30h/31h (see the test above) --
    // writing it afterward would put the compat register's own minimum back
    // into 30h rather than leaving the 0 written directly.
    MixerWrite(0x22, 0x00);
    MixerWrite(0x30, 0x00);
    ASSERT_EQ(MixerRead(0x30), 0x00);
    // The mixer is a separate chip: a DSP reset must leave its volumes alone,
    // or every driver's carefully set levels would vanish on card re-init.
    ResetDsp();
    EXPECT_EQ(MixerRead(0x30), 0x00);
    // Register 00h: "write any 8-bit value to this register to reset the mixer".
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
    // Real hardware has no such limit (the DAC just keeps converting); this
    // only bounds memory if the front end stops draining a card that is
    // actively playing -- same reasoning, and same shape, as
    // PcSpeaker::kMaxEdges.
    ResetDsp();
    constexpr int kMaxSamples = 1 << 16;
    for (int i = 0; i < kMaxSamples + 5; ++i) {
        sb.out(kWriteCmd, 0x10);
        sb.out(kWriteCmd, uint8_t(i & 0xFF));
    }
    auto samples = sb.drain_samples();
    ASSERT_EQ(samples.size(), std::size_t(kMaxSamples));
    // The surviving oldest sample is the one written at i == 5.
    EXPECT_EQ(samples.front().left, int16_t((5 - 128) * 256));
    EXPECT_EQ(samples.back().left, int16_t((((kMaxSamples + 4) & 0xFF) - 128) * 256));
}

}  // namespace
