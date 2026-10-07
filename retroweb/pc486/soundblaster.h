// Creative Sound Blaster 16 (CT1745 mixer, DSP 4.05), ISA: base 0x220, 20-port
// block 0x220-0x233, IRQ5, DMA1 (8-bit) and DMA5 (16-bit), i.e. the card
// `SET BLASTER=A220 I5 D1 H5 T6` describes. Programmed per Creative's "Sound
// Blaster Series Hardware Programming Guide" (1994, "SBPG"). Ports are base+0h..13h
// (SBPG Appendix A); Table 2-1 predates the mixer and base+Fh.
//   base+0h/1h  FM (OPL3) left status/address, data
//   base+2h/3h  FM (OPL3) right status/address, data
//   base+4h     Mixer register address (CT1745)
//   base+5h     Mixer data
//   base+6h     DSP reset                        (write only)
//   base+8h/9h  FM status/register, data (AdLib-compatible pair)
//   base+Ah     DSP read data                    (read only)
//   base+Ch     DSP write command/data (write) / write-buffer status (read)
// base+Eh     DSP read-buffer status (read only); also acks the 8-bit DMA IRQ
// base+Fh     16-bit DMA IRQ acknowledge (read only, DSP 4.xx; SBPG 2-5)
// base+10h-13h  the card's CD-ROM interface, decoded as present with no drive;
//                 the CD-ROM is on the IDE/ATAPI channel (wd1003.h/atapi_cdrom.h)
//
// SBPG ch.2: DSP reset is write 1, write 0 to base+6h, poll base+Eh bit 7,
// read 0AAh from base+Ah. Ack 8-bit IRQs by reading base+Eh, 16-bit by base+Fh.
// Mixer 82h reports the source, 80h selects the IRQ (bit 1 = IRQ5), 81h the DMA
// channels (bit 1 = DMA1, bit 5 = DMA5).
// DSP 4.05 is a real SB16 revision and load-bearing: Allegro 3.x sb.c (the BOOM
// driver) reads E1h as (major << 8) | minor and takes its SB16 path at >= 0x400
// (41h sample rate in Hz, B6h mode 30h 16-bit signed stereo auto-init). Lower
// and it falls back to the SB-Pro/SB-2 path.
//
// Commands: direct-mode 10h; 8-bit single-cycle 14h and auto-init 1Ch with
// 40h/48h (SBPG 3-13..3-16); DSP 4.xx Bxh/Cxh programmed 8/16-bit, mono/stereo,
// signed/unsigned transfers with 41h/42h rates (SBPG 3-26..3-29); pause/continue/
// exit D0h/D4h/D5h/D6h/D9h/DAh; speaker D1h/D3h/D8h; silence 80h; ADPCM output
// 16h/17h/74h-77h and auto-init 1Fh/7Dh/7Fh (SBPG Table 3-1); direct input 20h;
// and E0h, E1h, E2h, E3h, E4h, E8h, F2h, F3h for card and IRQ/DMA detection.
//
// Scope (PC486_REVIEW.md):
// - FM is a real YMF262 (opl3.h). base+0h/1h is bank 0, base+2h/3h bank 1,
//   base+8h/9h the AdLib alias; status reads at base+0h, 2h, 8h make AdLib
//   detection succeed.
// - 0x388/0x389 is decoded as bank 0: AdLib-era drivers (DOOM) write FM there and
//   never touch the card's block. 0x38Ah/0x38Bh (AdLib Gold/PAS bank 1) are not
//   decoded; the SB16 uses base+2h/3h.
// - ADPCM output plays all three ratios including the reference byte. The step
//   tables have no primary source (soundblaster.cpp kAdpcm*).
// - No SB-MIDI (30h/31h/34h-38h) or joystick. The MPU-401 is in mpu401.h.
// - High-speed commands 90h/91h/98h/99h are accepted and ignored: SBPG lists them
//   for DSP 2.01+ and 3.xx only. Allegro uses them only below version 4.00.
// - Write-buffer status at base+Ch always reads ready (7Fh). Real hardware is briefly
//   busy; polling drivers work either way.
// - Recording (24h/2Ch, C8h/CEh, B8h/BEh) is paced and delivers digital silence,
//   like a real card with nothing plugged in. Interrupts fire on schedule.
// - Pacing is the accumulated-credit scheme of fdc765.h: wall-clock time per sample
//   is exact, but a batch of bytes moves in one burst, so DMA address/count step
//   in bursts. Software polling the DMA count mid-block would see steps.
//
// --- chipset wiring ---
// Like fdc765.h, this never touches memory or the DMA controller. chipset.cpp
// moves bytes via transfer_ready()/transfer_length()/transfer_buffer()/
// finish_transfer() and raises irq_line() on the 0->1 edge of irq_pending().
// - A 16-bit transfer runs on DMA2 channel 5 (index 1), whose address and count
//   count WORDS: the 8237 drives A1-A16 and the page register A17-A23. Physical
//   address is (uint32_t(dma2.page(1)) << 16) | (uint32_t(dma2.address(1)) << 1),
//   bytes available (count + 1) * 2, one dma2.advance(1) per pair. Page-register
//   bit 0 is not connected on the 16-bit channels.
// - Auto-init expects the DMA controller to reload from its base registers at TC
//   (Dma8237 models this). The device keeps asking until DAh/D9h.
// - Direction follows the DSP A/D bit, opposite to the floppy's flag: playback
//   (transfer_is_input() false) reads memory into transfer_buffer(), recording
//   writes it out (PC486_REVIEW.md §13).
#ifndef PC486_SOUNDBLASTER_H
#define PC486_SOUNDBLASTER_H

#include "opl3.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc486 {

class SoundBlaster {
public:
    // Factory default base (SBPG Appendix A) and period-standard SB16 jumper defaults
    static constexpr uint16_t kDefaultBase = 0x220;
    static constexpr int kDefaultIrq = 5;
    static constexpr int kDefaultDma8 = 1;
    static constexpr int kDefaultDma16 = 5;
    // A real shipped SB16 DSP revision; Allegro's detection depends on it
    static constexpr uint8_t kDspMajor = 4;
    static constexpr uint8_t kDspMinor = 5;

    explicit SoundBlaster(uint16_t base = kDefaultBase) : base_(base) { reset(); }

    // The OPL3 behind base+0h..3h and base+8h/9h. Public so the front end drains
    // its samples directly: the card sums FM and digitized audio in the analog domain.
    Opl3 fm;

    // Cold power-on: DSP idle, mixer at CT1745 defaults (SBPG ch.4), IRQ5/DMA1/DMA5
    void reset();

    // --- chipset port decode ---
    // The 20-port block (SBPG Table A-15: base+0h..13h) plus the AdLib 0x388/0x389 FM pair
    static constexpr uint16_t kAdLibFmAddr = 0x388;
    static constexpr uint16_t kAdLibFmData = 0x389;
    bool owns(uint16_t port) const {
        return (port >= base_ && port <= uint16_t(base_ + 0x13)) ||
               port == kAdLibFmAddr || port == kAdLibFmData;
    }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advances pacing against the CPU's cycle count, like Fdc765::tick. Inline
    // with an early-out so a silent card is nearly free (PC486_REVIEW.md §8).
    void tick(uint64_t cpu_cycles) {
        // FM runs independently of DSP transfers, so it steps before the early-out
        fm.tick(cpu_cycles);
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (mode_ == Mode::kIdle || paused_) return;
        advance(cpu_cycles, delta);
    }

    // Turbo can drop the DX2 to 33 MHz; keeps pacing wall-clock correct (with fm)
    void set_cpu_hz(double hz);

    // --- interrupts ---
    // One IRQ line carries 8-bit DMA, 16-bit DMA and MIDI sources. Mixer 82h says
    // which fired; software acks by reading base+Eh (8-bit) or base+Fh (16-bit). SBPG 2-5.
    bool irq_pending() const { return (irq8_ || irq16_); }
    void clear_irq() { irq8_ = irq16_ = false; }  // real software acks via the ports above
    bool irq8_pending() const { return irq8_; }
    bool irq16_pending() const { return irq16_; }
    // Software-selected, from mixer register 80h; 5 out of reset.
    int irq_line() const;

    // --- DMA channel selection (mixer register 81h) ---
    int dma_channel_8bit() const;   // 1 out of reset
    int dma_channel_16bit() const;  // 5 out of reset

    // --- transfer handoff ---
    bool transfer_ready() const { return transfer_ready_; }
    // true = A/D, device -> memory (recording); false = D/A playback.
    bool transfer_is_input() const { return is_input_; }
    // true = 16-bit channel, whose registers count words (see wiring above)
    bool transfer_is_16bit() const { return bits16_; }
    bool transfer_is_autoinit() const { return autoinit_; }
    int transfer_dma_channel() const { return bits16_ ? dma_channel_16bit() : dma_channel_8bit(); }
    std::size_t transfer_length() const { return transfer_len_; }   // bytes
    uint8_t *transfer_buffer() { return buffer_.data(); }
    // `actual_len` is the bytes the chipset really moved; fewer than
    // transfer_length() when the DMA count ran out first (terminal count)
    void finish_transfer(std::size_t actual_len);

    // --- audio output ---
    // Like PcSpeaker's edge log: records what the DAC latched and when. Each sample
    // carries its CPU cycle so a Web Audio renderer resamples from real time and
    // rate changes need no special case. Normalized to signed 16-bit stereo: 8-bit
    // unsigned is centered and scaled, mono duplicated.
    struct Sample {
        uint64_t cpu_cycle;
        int16_t left, right;
    };
    std::vector<Sample> drain_samples();

    // What the DAC is doing, for the front end and tests
    uint32_t sample_rate_hz() const { return rate_hz_; }
    bool playing() const { return mode_ != Mode::kIdle && !paused_; }
    bool stereo() const { return stereo_; }
    bool sixteen_bit() const { return bits16_; }
    bool speaker_on() const { return speaker_on_; }

    // The analog chain after the samples: CT1745 Voice and Master attenuators
    // (SBPG ch.4: 5-bit, 0-31 = -62 dB to 0 dB in 2 dB steps, default 24), then
    // Output Gain .L/.R (mixer 41h/42h: 0-3 = 0 to 18 dB in 6 dB steps), as linear
    // gains. Kept out of the sample path so drain_samples() returns what the program wrote.
    float output_gain_left() const;
    float output_gain_right() const;
    // The FM leg: the OPL3 is the internal MIDI source, so mixer 34h/35h ("MIDI
    // volume") into Master, not the Voice pair
    float fm_gain_left() const;
    float fm_gain_right() const;
    // The analog CD leg: mixer 36h/37h ("CD volume") into Master, gated by 3Ch
    // (CD.L = bit 2, CD.R = bit 1; SBPG ch.4). Where atapi_cdrom.h's CD-DA output goes.
    float cd_gain_left() const;
    float cd_gain_right() const;
    uint8_t mixer_register(uint8_t index) const { return mixer_[index]; }

private:
    // kIdentify: the one-byte E2h DMA-identification write, a single unpaced DMA
    // cycle that doesn't fit kDma's block accounting
    enum class Mode { kIdle, kDma, kSilence, kIdentify };

    // The three ADPCM ratios (SBPG 3-7); kNone is ordinary PCM
    enum class Adpcm { kNone, k2Bit, k3Bit, k4Bit };

    // 256 KB, more than any period driver programs (the 8237 addresses 64 KB)
    static constexpr std::size_t kBufferBytes = 256u * 1024u;
    // Same bound as PcSpeaker::kMaxEdges; caps memory if the front end stops draining
    static constexpr std::size_t kMaxSamples = 1u << 16;
    // Turbo-on default; set_cpu_hz() updates the live rate
    static constexpr double kCpuHz = 66000000.0;
    double cpu_hz_ = kCpuHz;

    void advance(uint64_t cpu_cycles, uint64_t delta);
    void dsp_write(uint8_t v);
    void run_command();
    void reset_dsp(bool from_reset_port);
    void reset_mixer();
    void mixer_write(uint8_t index, uint8_t v);
    uint8_t mixer_read(uint8_t index) const;
    // `dma_units` is the programmed length (+1'd): bytes on the 8-bit channel, words on the 16-bit
    void begin_dma(bool input, bool is16, bool ai, bool stereo, bool signed_data,
                   uint32_t dma_units);
    // ADPCM output commands (16h/17h/74h-77h, auto-init 1Fh/7Dh/7Fh): 8-bit mono
    // on the 8-bit channel, output only (SBPG 3-7). `need_ref` marks commands whose
    // first block byte is a reference value.
    void begin_adpcm(Adpcm format, bool ai, bool need_ref, uint32_t dma_bytes);
    // Decodes one byte into frames_per_byte() samples, advancing the predictor/step state
    void decode_adpcm_byte(uint8_t byte, uint64_t cycle, double cycles_per_frame);
    void push_sample(uint64_t cycle, int16_t l, int16_t r);
    void raise_block_irq();
    void fill_input_buffer(std::size_t len);
    int bytes_per_frame() const { return (bits16_ ? 2 : 1) * (stereo_ ? 2 : 1); }
    // Samples per compressed byte: 2 for 4-bit, 3 for 3-bit (the "2.6-bit" mode,
    // last sample two bits wide), 4 for 2-bit. PCM is one frame per byte.
    int frames_per_byte() const {
        switch (adpcm_) {
            case Adpcm::k4Bit: return 2;
            case Adpcm::k3Bit: return 3;
            case Adpcm::k2Bit: return 4;
            default: return 1;
        }
    }

    uint16_t base_;

    uint8_t cmd_ = 0;
    uint8_t params_[4] = {};
    int params_needed_ = 0, params_got_ = 0;
    std::deque<uint8_t> read_fifo_;
    uint8_t last_read_ = 0;   // real hardware re-reads the last byte from a drained FIFO
    uint8_t test_reg_ = 0;    // E4h/E8h diagnostic register; survives a DSP reset
    bool reset_asserted_ = false;
    bool speaker_on_ = false;
    // E2h DMA-identification state, reset on every DSP reset (undocumented by Creative)
    uint8_t ident_valadd_ = 0xAA;
    uint8_t ident_valxor_ = 0x96;

    Mode mode_ = Mode::kIdle;
    bool is_input_ = false, bits16_ = false, autoinit_ = false, stereo_ = false;
    bool signed_data_ = false;
    bool paused_ = false;
    bool exit_autoinit_ = false;
    uint32_t rate_hz_ = 11025;      // no meaningful default on real hardware; a
                                    // driver always sets 40h or 41h before playing
    uint32_t block_frames_ = 0;     // 48h / Bxh-Cxh length, in frames
    uint32_t block_left_ = 0;

    // ADPCM blocks count compressed bytes in block_frames_/block_left_, as the DSP length parameter does
    Adpcm adpcm_ = Adpcm::kNone;
    bool adpcm_need_ref_ = false;  // consume the next byte as the reference
    uint8_t adpcm_ref_ = 0;        // last decoded sample, the decoder's predictor
    uint8_t adpcm_step_ = 0;       // adaptive step-size index
    uint16_t dsp_block_size_ = 0;   // last 48h value, in samples-1 as programmed

    bool transfer_ready_ = false;
    std::size_t transfer_len_ = 0;
    uint64_t xfer_start_cycle_ = 0;
    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kBufferBytes, 0);

    double frame_credit_ = 0.0;   // elapsed CPU cycles not yet turned into frames
    uint64_t prev_cycles_ = 0;

    bool irq8_ = false, irq16_ = false;

    std::deque<Sample> samples_;

    uint8_t mixer_index_ = 0;
    uint8_t mixer_[256] = {};
};

}  // namespace pc486

#endif  // PC486_SOUNDBLASTER_H
