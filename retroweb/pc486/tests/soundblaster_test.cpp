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

TEST_F(SoundBlasterTest, DecodesTheWholeSixteenPortBlockAndNothingElse) {
    // A real card decodes base+0h..base+Fh (SBPG Appendix A), all sixteen
    // ports, not just the DSP's four.
    for (uint16_t p = 0x220; p <= 0x22F; ++p) EXPECT_TRUE(sb.owns(p)) << std::hex << p;
    EXPECT_FALSE(sb.owns(0x21F));
    EXPECT_FALSE(sb.owns(0x230));
    // 0x388/0x389, the alternate FM address pair, are deliberately NOT
    // claimed: no FM synthesizer is implemented, and claiming the ports
    // would let AdLib detection succeed against silence. See soundblaster.h.
    EXPECT_FALSE(sb.owns(0x388));
    EXPECT_FALSE(sb.owns(0x389));
}

TEST_F(SoundBlasterTest, AdlibDetectionFailsBecauseNoFmSynthesizerIsPresent) {
    // The standard AdLib probe: reset timers 1 and 2, read status (expects
    // 00h), start timer 1, read status again and expect C0h. The second read
    // must not produce C0h here -- see soundblaster.h on why a fake OPL that
    // passed this would be worse than absent hardware.
    sb.out(kBase + 0x08, 0x04); sb.out(kBase + 0x09, 0x60);  // reset both timers
    sb.out(kBase + 0x08, 0x04); sb.out(kBase + 0x09, 0x80);  // reset IRQ flags
    EXPECT_EQ(sb.in(kBase + 0x08) & 0xE0, 0x00);
    sb.out(kBase + 0x08, 0x02); sb.out(kBase + 0x09, 0xFF);  // timer 1 preset
    sb.out(kBase + 0x08, 0x04); sb.out(kBase + 0x09, 0x21);  // start timer 1
    EXPECT_NE(sb.in(kBase + 0x08) & 0xE0, 0xC0);
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

// --- DSP 4.xx programmed transfers --------------------------------------

TEST_F(SoundBlasterTest, SixteenBitSignedStereoAutoInitUsesChannelFiveAndTheSixteenBitIrq) {
    // Exactly what Allegro's SB16 path issues: 41h with the rate, then B6h
    // (16-bit, auto-init, FIFO on) with mode 30h (16-bit signed stereo).
    ResetDsp();
    Cmd({0x41, 0xAC, 0x44});      // 44100 Hz
    Cmd({0xB6, 0x30, 0x03, 0x00});  // 4 frames per block
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

TEST_F(SoundBlasterTest, EightBitUnsignedStereoSingleCycleViaCxCommand) {
    ResetDsp();
    Cmd({0x41, 0x2B, 0x11});        // 11025 Hz exactly
    EXPECT_EQ(sb.sample_rate_hz(), 11025u);
    Cmd({0xC0, 0x20, 0x01, 0x00});  // 8-bit, single-cycle, mode 20h = stereo unsigned
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

TEST_F(SoundBlasterTest, AdpcmCommandsSwallowTheirParametersWithoutDesyncing) {
    // ADPCM playback is out of scope, but a driver probing 75h must not be
    // able to knock the command stream out of step (its two length bytes
    // would otherwise be read as commands). See soundblaster.h.
    ResetDsp();
    Cmd({0x75, 0xFF, 0x01});
    EXPECT_FALSE(sb.playing());
    Cmd({0xE1});
    EXPECT_EQ(Read(), 4);
    EXPECT_EQ(Read(), 5);
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
