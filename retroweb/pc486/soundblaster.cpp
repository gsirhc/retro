#include "soundblaster.h"

#include <algorithm>
#include <cmath>

namespace pc486 {

namespace {

// Real SB16 DSPs answer command E3h with this exact string (terminated by a
// null byte); period drivers read it to tell a genuine Creative card from a
// clone. Text as returned by real hardware, per DOSBox/DOSBox-X's
// sblaster.cpp `copyright_string`.
constexpr char kCopyright[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

// The rate registers themselves only have to hold a value the DSP can clock
// at all; which range actually applies depends on the format the transfer
// command then selects, so the per-format limit is imposed when a transfer
// starts (see clamp_rate_for_format) rather than here. 4000 Hz is the lowest
// figure anywhere in SBPG Tables 3-2/3-3 (the ADPCM rows) and 44100 Hz the
// highest; real hardware clamps rather than rejecting an out-of-range value.
uint32_t clamp_rate(uint32_t hz) {
    if (hz < 4000) return 4000;
    if (hz > 44100) return 44100;
    return hz;
}

// SBPG chapter 3, "Digitized Sound I/O Transfer Rate":
//   Time Constant = 65536 - (256 000 000 / (channels * sampling rate))
// with only the high byte programmed through command 40h, which reduces to
// tc = 256 - (1 000 000 / (channels * rate)). The driver is the one that
// folds `channels` in before writing the byte (Allegro's sb.c writes
// `256 - 1000000/rate` outright), so inverting it here gives back the rate
// the DSP clocks samples at. A time constant of 255 inverts to 1 MHz, well
// past the 44100 Hz ceiling, so the caller clamps the result (see Tables
// 3-2/3-3 above).
uint32_t time_constant_to_rate(uint8_t tc) {
    return uint32_t(1000000 / (256 - int(tc)));
}

// SBPG's Bxh/Cxh command pages: "For minimum signal amplitude, the signed
// 8-bit value is 00h; with unsigned data, the equivalent value is 80h" (and
// 0000h/8000h for 16-bit) -- i.e. the two formats differ only by where
// silence sits, so centering is all the conversion needs.
int16_t expand8(uint8_t b, bool is_signed) {
    int v = is_signed ? int(int8_t(b)) : int(b) - 128;
    return int16_t(v * 256);  // scale, not a shift: v is signed and can be negative
}
int16_t expand16(uint8_t lo, uint8_t hi, bool is_signed) {
    uint16_t u = uint16_t(uint16_t(hi) << 8 | lo);
    return is_signed ? int16_t(u) : int16_t(int(u) - 0x8000);
}

// CT1745 volume fields are 5 bits, left-justified in the byte: 0-31 maps to
// -62 dB..0 dB in 2 dB steps (SBPG chapter 4).
float five_bit_gain(uint8_t reg) {
    int level = reg >> 3;
    return std::pow(10.0f, (-62.0f + 2.0f * float(level)) / 20.0f);
}

// Output Gain .L/.R (mixer 41h/42h): 2 bits, left-justified, 0-3 => 0 dB to
// 18 dB in 6 dB steps (SBPG chapter 4). Unlike the attenuator fields above,
// this stage boosts rather than cuts.
float two_bit_boost_gain(uint8_t reg) {
    int level = reg >> 6;
    return std::pow(10.0f, (6.0f * float(level)) / 20.0f);
}

// ADPCM decode tables. Creative never published the DSP's step tables, and
// the command-reference chapter is missing from the copy of SBPG this repo
// works from, so these are transcribed from DOSBox-X's sblaster.cpp
// (`scaleMap_ADPCM*`/`adjustMap_ADPCM*`) -- second-hand corroboration, not a
// primary source, flagged the same way begin_dma's block-counter note is.
// The decode itself is one step per sample: index the table with
// (code + step size), add the scale delta to the running reference, then
// adjust the step size. Entry count differs per ratio because the index
// space is (codes x step-size levels).
constexpr int8_t kAdpcm4Scale[64] = {
    0,  1,  2,  3,  4,  5,  6,  7,  0,  -1,  -2,  -3,  -4,  -5,  -6,  -7,
    1,  3,  5,  7,  9, 11, 13, 15, -1,  -3,  -5,  -7,  -9, -11, -13, -15,
    2,  6, 10, 14, 18, 22, 26, 30, -2,  -6, -10, -14, -18, -22, -26, -30,
    4, 12, 20, 28, 36, 44, 52, 60, -4, -12, -20, -28, -36, -44, -52, -60,
};
constexpr uint8_t kAdpcm4Adjust[64] = {
      0, 0, 0, 0, 0, 16, 16, 16,
      0, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0,  0,  0,  0,
    240, 0, 0, 0, 0,  0,  0,  0,
};
constexpr int8_t kAdpcm3Scale[40] = {
    0,  1,  2,  3,  0,  -1,  -2,  -3,
    1,  3,  5,  7, -1,  -3,  -5,  -7,
    2,  6, 10, 14, -2,  -6, -10, -14,
    4, 12, 20, 28, -4, -12, -20, -28,
    5, 15, 25, 35, -5, -15, -25, -35,
};
constexpr uint8_t kAdpcm3Adjust[40] = {
      0, 0, 0, 8,   0, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 8, 248, 0, 0, 8,
    248, 0, 0, 0, 248, 0, 0, 0,
};
constexpr int8_t kAdpcm2Scale[24] = {
    0,  1,  0,  -1,  1,  3,  -1,  -3,
    2,  6, -2,  -6,  4, 12,  -4, -12,
    8, 24, -8, -24, 16, 48, -16, -48,
};
constexpr uint8_t kAdpcm2Adjust[24] = {
      0, 4,   0, 4,
    252, 4, 252, 4, 252, 4, 252, 4,
    252, 4, 252, 4, 252, 4, 252, 4,
    252, 0, 252, 0,
};

// The sampling range for the format a transfer command just selected, per
// SBPG Table 3-2 (output) and 3-3 (input): PCM on DSP 4.xx runs 5000-44100 Hz
// in every width and channel count, while the ADPCM ratios each stop lower --
// 4-bit at 12000, 3-bit at 13000, 2-bit at 11000, all from 4000.
uint32_t clamp_rate_for_format(uint32_t hz, int adpcm_bits) {
    uint32_t lo = 5000, hi = 44100;
    switch (adpcm_bits) {
        case 4: lo = 4000; hi = 12000; break;
        case 3: lo = 4000; hi = 13000; break;
        case 2: lo = 4000; hi = 11000; break;
        default: break;
    }
    if (hz < lo) return lo;
    if (hz > hi) return hi;
    return hz;
}

// One ADPCM sample: `code` is the compressed code, `ref`/`step` the running
// decoder state. Returns the decoded unsigned-8-bit sample, which is also
// the new reference.
uint8_t decode_adpcm(uint8_t code, const int8_t *scale, const uint8_t *adjust, int entries,
                     uint8_t &ref, uint8_t &step) {
    int idx = int(code) + int(step);
    if (idx < 0) idx = 0;
    if (idx >= entries) idx = entries - 1;
    int next = int(ref) + int(scale[idx]);
    if (next < 0) next = 0;
    if (next > 255) next = 255;
    ref = uint8_t(next);
    step = uint8_t(step + adjust[idx]);
    return ref;
}

// The CT1345-compatibility volume registers (04h/22h/26h/28h/2Eh) are, in
// Creative's words, "actually mapped to the new volume control registers":
// 4 bits per channel at 4 dB steps against the new registers' 5 bits at 2 dB
// steps. n -> 2n+1 is the exact mapping, since -60 + 4n == -62 + 2(2n+1).
uint8_t compat_to_new(uint8_t nibble) { return uint8_t(((nibble & 0x0F) * 2 + 1) << 3); }
uint8_t new_to_compat(uint8_t reg) { return uint8_t((reg >> 3) >> 1); }

int lowest_set_bit_channel(uint8_t v, const int *map, int count) {
    for (int i = 0; i < count; ++i)
        if (map[i] >= 0 && (v & (1 << i))) return map[i];
    return -1;
}

}  // namespace

void SoundBlaster::reset() {
    fm.reset();
    reset_dsp(false);
    reset_mixer();
    test_reg_ = 0;  // cleared only by a cold power-on; a DSP reset leaves it alone
    samples_.clear();
    frame_credit_ = 0.0;
    prev_cycles_ = 0;
    reset_asserted_ = false;
}

void SoundBlaster::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    frame_credit_ *= hz / cpu_hz_;
    cpu_hz_ = hz;
    fm.set_cpu_hz(hz);
}

void SoundBlaster::reset_dsp(bool from_reset_port) {
    mode_ = Mode::kIdle;
    is_input_ = bits16_ = autoinit_ = stereo_ = signed_data_ = false;
    paused_ = false;
    exit_autoinit_ = false;
    block_frames_ = block_left_ = 0;
    transfer_ready_ = false;
    transfer_len_ = 0;
    cmd_ = 0;
    params_needed_ = params_got_ = 0;
    read_fifo_.clear();
    speaker_on_ = false;
    irq8_ = irq16_ = false;
    frame_credit_ = 0.0;
    ident_valadd_ = 0xAA;
    ident_valxor_ = 0x96;
    adpcm_ = Adpcm::kNone;
    adpcm_need_ref_ = false;
    adpcm_ref_ = 0x80;
    adpcm_step_ = 0;
    // SBPG 2-2: after the reset handshake "the DSP returns a data byte 0AAh
    // at the Read Data port" -- the single byte every driver's card-detection
    // routine is waiting for. A reset_dsp() done for any other reason (cold
    // power-on) must not fabricate it.
    if (from_reset_port) read_fifo_.push_back(0xAA);
    // The mixer is a separate chip (CT1745) and is deliberately untouched
    // here: a DSP reset does not restore mixer volumes on real hardware.
}

void SoundBlaster::reset_mixer() {
    for (auto &r : mixer_) r = 0;
    // Defaults per SBPG chapter 4's register-by-register list.
    for (uint8_t i = 0x30; i <= 0x35; ++i) mixer_[i] = 24 << 3;  // master/voice/MIDI: 24 => -14 dB
    // 0x36-0x3A (CD, line, mic) default to 0 => -62 dB, 0x3B (PC speaker) to 0.
    mixer_[0x3C] = 0x1F;  // output switches: Line.L/.R, CD.L/.R, Mic all closed
    mixer_[0x3D] = 0x15;  // input mixer L: Line.L, CD.L, Mic
    mixer_[0x3E] = 0x0B;  // input mixer R: Line.R, CD.R, Mic
    // 0x3F-0x42 input/output gain default 0 => 0 dB; 0x43 bit 0 clear => mic AGC on.
    for (uint8_t i = 0x44; i <= 0x47; ++i) mixer_[i] = 8 << 4;  // treble/bass: 8 => 0 dB
    mixer_[0x80] = 0x02;  // Interrupt Setup: bit 1 = IRQ5
    mixer_[0x81] = 0x22;  // DMA Setup: bit 1 = DMA1 (8-bit), bit 5 = DMA5 (16-bit)
}

int SoundBlaster::irq_line() const {
    // SBPG 2-6, mixer register 80h: D0=IRQ2, D1=IRQ5, D2=IRQ7, D3=IRQ10.
    // "Note that only a bit can be set on at any one time"; -1 means software
    // has deselected every line, so the card drives no interrupt at all.
    static const int kMap[] = {2, 5, 7, 10};
    return lowest_set_bit_channel(mixer_[0x80], kMap, 4);
}

int SoundBlaster::dma_channel_8bit() const {
    // SBPG 2-7, mixer register 81h: D0=DMA0, D1=DMA1, D3=DMA3.
    static const int kMap[] = {0, 1, -1, 3};
    return lowest_set_bit_channel(mixer_[0x81], kMap, 4);
}

int SoundBlaster::dma_channel_16bit() const {
    // Same register, D5=DMA5, D6=DMA6, D7=DMA7.
    static const int kMap[] = {-1, -1, -1, -1, -1, 5, 6, 7};
    return lowest_set_bit_channel(mixer_[0x81], kMap, 8);
}

float SoundBlaster::output_gain_left() const {
    return five_bit_gain(mixer_[0x30]) * five_bit_gain(mixer_[0x32]) *
           two_bit_boost_gain(mixer_[0x41]);
}
float SoundBlaster::output_gain_right() const {
    return five_bit_gain(mixer_[0x31]) * five_bit_gain(mixer_[0x33]) *
           two_bit_boost_gain(mixer_[0x42]);
}

float SoundBlaster::fm_gain_left() const {
    // Output Gain sits after the mixer, so it applies to the FM leg too.
    return five_bit_gain(mixer_[0x30]) * five_bit_gain(mixer_[0x34]) *
           two_bit_boost_gain(mixer_[0x41]);
}
float SoundBlaster::fm_gain_right() const {
    return five_bit_gain(mixer_[0x31]) * five_bit_gain(mixer_[0x35]) *
           two_bit_boost_gain(mixer_[0x42]);
}

uint8_t SoundBlaster::mixer_read(uint8_t index) const {
    switch (index) {
        case 0x82: {
            // Interrupt Status (SBPG 2-5): D0 = 8-bit DMA digitized sound or
            // SB-MIDI, D1 = 16-bit DMA digitized sound, D2 = MPU-401. An ISR
            // reads this first to decide whether the interrupt is even its own.
            uint8_t v = 0;
            if (irq8_) v = uint8_t(v | 0x01);
            if (irq16_) v = uint8_t(v | 0x02);
            return v;
        }
        // CT1345-compatibility volume registers, read back from the new
        // registers they alias (see compat_to_new).
        case 0x04: return uint8_t(new_to_compat(mixer_[0x32]) << 4 | new_to_compat(mixer_[0x33]));
        case 0x22: return uint8_t(new_to_compat(mixer_[0x30]) << 4 | new_to_compat(mixer_[0x31]));
        case 0x26: return uint8_t(new_to_compat(mixer_[0x34]) << 4 | new_to_compat(mixer_[0x35]));
        case 0x28: return uint8_t(new_to_compat(mixer_[0x36]) << 4 | new_to_compat(mixer_[0x37]));
        case 0x2E: return uint8_t(new_to_compat(mixer_[0x38]) << 4 | new_to_compat(mixer_[0x39]));
        default: return mixer_[index];
    }
}

void SoundBlaster::mixer_write(uint8_t index, uint8_t v) {
    switch (index) {
        case 0x00: reset_mixer(); return;  // "Write any 8-bit value to this register to reset the mixer"
        case 0x82: return;                 // Interrupt Status is read-only
        case 0x04: mixer_[0x32] = compat_to_new(uint8_t(v >> 4)); mixer_[0x33] = compat_to_new(v); return;
        case 0x22: mixer_[0x30] = compat_to_new(uint8_t(v >> 4)); mixer_[0x31] = compat_to_new(v); return;
        case 0x26: mixer_[0x34] = compat_to_new(uint8_t(v >> 4)); mixer_[0x35] = compat_to_new(v); return;
        case 0x28: mixer_[0x36] = compat_to_new(uint8_t(v >> 4)); mixer_[0x37] = compat_to_new(v); return;
        case 0x2E: mixer_[0x38] = compat_to_new(uint8_t(v >> 4)); mixer_[0x39] = compat_to_new(v); return;
        // 0x0A (mono mic volume) is stored as written rather than aliased
        // into 0x3A: its 3-bit/6 dB scale has no exact mapping onto the new
        // register's 2 dB steps, and it sits outside the digitized-output
        // path this milestone reproduces.
        default: mixer_[index] = v; return;
    }
}

uint8_t SoundBlaster::in(uint16_t port) {
    // The AdLib pair sits outside the card's own block, so it is decoded
    // before the base-relative switch below.
    if (port == kAdLibFmAddr) return fm.status();
    if (port == kAdLibFmData) return 0xFF;  // data port: write-only, open bus
    switch (port - base_) {
        // FM status. All three of the chip's status ports return the same
        // byte on real hardware -- there is one status register, not one per
        // bank -- and base+8h is simply the AdLib alias of base+0h.
        case 0x00: case 0x02: case 0x08: return fm.status();
        case 0x04: return mixer_index_;
        case 0x05: return mixer_read(mixer_index_);
        case 0x0A: {  // DSP Read Data
            if (!read_fifo_.empty()) {
                last_read_ = read_fifo_.front();
                read_fifo_.pop_front();
            }
            // A drained FIFO keeps handing back the last byte on real
            // hardware rather than reading as open bus.
            return last_read_;
        }
        case 0x0C:
            // Write-Buffer Status: bit 7 clear means "ready to accept a
            // command or data byte" (SBPG 2-4). Always ready here -- see the
            // file header's note on the real busy cycle.
            return 0x7F;
        case 0x0E:
            // Read-Buffer Status: bit 7 set means a byte is waiting (SBPG
            // 2-3). Reading this port is also how 8-bit DMA-mode digitized
            // sound interrupts are acknowledged, "to remain backward
            // compatible" (SBPG 2-5) -- so the acknowledge happens on every
            // read, including the harmless ones a reset poll does.
            irq8_ = false;
            return read_fifo_.empty() ? 0x7F : 0xFF;
        case 0x0F:
            // 16-bit DMA-mode interrupt acknowledge (SBPG 2-5). The value
            // read is not specified; real cards return FFh.
            irq16_ = false;
            return 0xFF;
        // base+10h-13h: the card's own proprietary CD-ROM interface
        // (Command/Data, Status, Reset, Enable). This machine's CD-ROM is on
        // the IDE/ATAPI channel instead (see chipset.h), so the card is
        // present but nothing answers behind it -- open bus.
        case 0x10: case 0x11: case 0x12: case 0x13: return 0xFF;
        default: return 0xFF;  // write-only or reserved port in the block: open bus
    }
}

void SoundBlaster::out(uint16_t port, uint8_t v) {
    if (port == kAdLibFmAddr) { fm.write_address(0, v); return; }
    if (port == kAdLibFmData) { fm.write_data(0, v); return; }
    switch (port - base_) {
        // FM: base+0h/1h is bank 0, base+2h/3h bank 1, base+8h/9h the
        // AdLib-compatible alias of bank 0.
        case 0x00: case 0x08: fm.write_address(0, v); return;
        case 0x01: case 0x09: fm.write_data(0, v); return;
        case 0x02: fm.write_address(1, v); return;
        case 0x03: fm.write_data(1, v); return;
        case 0x04: mixer_index_ = v; return;
        case 0x05: mixer_write(mixer_index_, v); return;
        case 0x06:
            // SBPG 2-2: write a 1, wait ~3us, write a 0; the DSP then posts
            // 0AAh. Only the 1 -> 0 transition completes a reset, which is
            // why a driver that leaves the card parked in reset (writing 1
            // and stopping) gets no acknowledge byte. But the DSP is held in
            // reset for as long as the 1 stands, so any transfer in progress
            // stops right there, not at the eventual 0 -- the mixer (a
            // separate chip) and test_reg_ are untouched either way.
            if (v & 0x01) {
                reset_asserted_ = true;
                mode_ = Mode::kIdle;
                transfer_ready_ = false;
                transfer_len_ = 0;
                block_frames_ = block_left_ = 0;
                paused_ = false;
            } else if (reset_asserted_) {
                reset_asserted_ = false;
                reset_dsp(true);
            }
            return;
        case 0x0C: dsp_write(v); return;
        // base+10h-13h: the card's own CD-ROM interface, present but with no
        // drive attached (see the file header) -- writes vanish, same as any
        // other reserved port in the block.
        case 0x10: case 0x11: case 0x12: case 0x13: return;
        default: return;  // reserved port in the block: write vanishes
    }
}

void SoundBlaster::dsp_write(uint8_t v) {
    if (params_got_ < params_needed_) {
        params_[params_got_++] = v;
        if (params_got_ >= params_needed_) run_command();
        return;
    }
    cmd_ = v;
    params_got_ = 0;
    switch (v) {
        case 0x10: params_needed_ = 1; break;                       // direct DAC output
        case 0x14: case 0x24:                                       // 8-bit single-cycle out/in
        case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:  // ADPCM single-cycle
        case 0x41: case 0x42:                                       // output/input sampling rate
        case 0x48:                                                  // block transfer size
        case 0x80: params_needed_ = 2; break;                       // silence period
        case 0x40: case 0x38: case 0xE0: case 0xE2: case 0xE4: params_needed_ = 1; break;
        default:
            // Bxh/Cxh take a mode byte plus a 16-bit length; everything else
            // this device answers is parameterless.
            params_needed_ = ((v & 0xF0) == 0xB0 || (v & 0xF0) == 0xC0) ? 3 : 0;
            break;
    }
    if (params_needed_ == 0) run_command();
}

void SoundBlaster::run_command() {
    const uint16_t len16 = uint16_t(uint16_t(params_[1]) << 8 | params_[0]);
    switch (cmd_) {
        case 0x10:  // 8-bit direct mode output: the application paces this itself
            push_sample(prev_cycles_, expand8(params_[0], false), expand8(params_[0], false));
            break;
        case 0x20:  // 8-bit direct mode input: nothing is plugged into the
                    // line/mic inputs, so the one sample byte returned is
                    // unsigned-8-bit silence -- same reasoning as
                    // fill_input_buffer.
            read_fifo_.push_back(0x80);
            break;

        // Legacy 8-bit mono unsigned PCM, always paced by the 40h time
        // constant (SBPG 3-13 and 3-15). wLength/wBlkSize are "one less than
        // the actual number of bytes", hence the +1.
        case 0x14: begin_dma(false, false, false, false, false, uint32_t(len16) + 1); break;
        case 0x24: begin_dma(true, false, false, false, false, uint32_t(len16) + 1); break;
        case 0x1C: begin_dma(false, false, true, false, false, uint32_t(dsp_block_size_) + 1); break;
        case 0x2C: begin_dma(true, false, true, false, false, uint32_t(dsp_block_size_) + 1); break;

        // ADPCM output. SBPG Table 3-1 lists 8-bit mono ADPCM single-cycle
        // AND auto-initialize for DSP 4.xx, so these are a real capability of
        // this card, not a pre-4.xx legacy. The "Reference" variants carry an
        // actual sample value as the first byte of the block instead of
        // compressed codes (SBPG 3-7). wLength is one less than the number of
        // compressed bytes, hence the +1.
        case 0x16: begin_adpcm(Adpcm::k2Bit, false, false, uint32_t(len16) + 1); break;
        case 0x17: begin_adpcm(Adpcm::k2Bit, false, true, uint32_t(len16) + 1); break;
        case 0x74: begin_adpcm(Adpcm::k4Bit, false, false, uint32_t(len16) + 1); break;
        case 0x75: begin_adpcm(Adpcm::k4Bit, false, true, uint32_t(len16) + 1); break;
        case 0x76: begin_adpcm(Adpcm::k3Bit, false, false, uint32_t(len16) + 1); break;
        case 0x77: begin_adpcm(Adpcm::k3Bit, false, true, uint32_t(len16) + 1); break;
        // The auto-init forms take their length from the last 48h, like 1Ch
        // and 2Ch do, and all three documented ones carry a reference byte.
        case 0x1F: begin_adpcm(Adpcm::k2Bit, true, true, uint32_t(dsp_block_size_) + 1); break;
        case 0x7D: begin_adpcm(Adpcm::k4Bit, true, true, uint32_t(dsp_block_size_) + 1); break;
        case 0x7F: begin_adpcm(Adpcm::k3Bit, true, true, uint32_t(dsp_block_size_) + 1); break;

        // High-speed mode does not exist on DSP 4.xx (SBPG chapter 6's
        // availability matrix lists 90h/91h/98h/99h for 2.01+ and 3.xx only),
        // so a real SB16 ignores these exactly as this does.
        case 0x90: case 0x91: case 0x98: case 0x99: break;
        // A0h/A8h (mono/stereo input select) likewise "no longer exists on
        // DSP version 4.xx"; stereo is selected by the Bxh/Cxh mode byte.
        case 0xA0: case 0xA8: break;

        case 0x40: rate_hz_ = clamp_rate(time_constant_to_rate(params_[0])); break;
        // 41h/42h carry the true sampling rate in Hz, HIGH byte first --
        // the opposite order from every length parameter on the card.
        case 0x41: case 0x42:
            rate_hz_ = clamp_rate(uint32_t(uint32_t(params_[0]) << 8 | params_[1]));
            break;
        case 0x48: dsp_block_size_ = len16; break;

        case 0x80:  // Pause DAC for wDuration+1 sampling periods, then interrupt
            mode_ = Mode::kSilence;
            block_frames_ = block_left_ = uint32_t(len16) + 1;
            bits16_ = stereo_ = signed_data_ = is_input_ = autoinit_ = false;
            paused_ = false;
            frame_credit_ = 0.0;
            break;

        default:
            if ((cmd_ & 0xF0) == 0xB0 || (cmd_ & 0xF0) == 0xC0) {
                // SBPG's Bxh command page: bit 3 selects A/D over D/A, bit 2
                // auto-init over single-cycle, bit 1 the FIFO (which affects
                // buffering inside the card, not the data, so it is read and
                // ignored here). The high nibble picks 16-bit (Bxh) or 8-bit
                // (Cxh). bMode bit 5 is stereo, bit 4 signed.
                const bool is16 = (cmd_ & 0xF0) == 0xB0;
                const bool input = (cmd_ & 0x08) != 0;
                const bool ai = (cmd_ & 0x04) != 0;
                const uint8_t mode = params_[0];
                const uint32_t dma_units =
                    uint32_t(uint16_t(uint16_t(params_[2]) << 8 | params_[1])) + 1;
                begin_dma(input, is16, ai, (mode & 0x20) != 0, (mode & 0x10) != 0, dma_units);
                break;
            }
            switch (cmd_) {
                // D0h/D4h act on 8-bit transfers and D5h/D6h on 16-bit ones
                // (SBPG chapter 6); a real card ignores the pair that does
                // not match the transfer currently running.
                case 0xD0: if (!bits16_) paused_ = true; break;
                case 0xD4: if (!bits16_) paused_ = false; break;
                case 0xD5: if (bits16_) paused_ = true; break;
                case 0xD6: if (bits16_) paused_ = false; break;
                // On DSP 4.xx D1h/D3h have "no practical effect on the output
                // signal" but still move the flag D8h reports.
                case 0xD1: speaker_on_ = true; break;
                case 0xD3: speaker_on_ = false; break;
                case 0xD8: read_fifo_.push_back(speaker_on_ ? 0xFF : 0x00); break;
                // Exit at the end of the current block, not immediately.
                case 0xD9: if (bits16_) exit_autoinit_ = true; break;
                case 0xDA: if (!bits16_) exit_autoinit_ = true; break;
                case 0xE0: read_fifo_.push_back(uint8_t(~params_[0])); break;  // DSP identification
                case 0xE2:
                    // DMA identification: undocumented by Creative -- the
                    // only description anywhere is DOSBox/DOSBox-X's
                    // sblaster.cpp, so this is second-hand corroboration,
                    // not a primary source (same caveat as begin_dma's
                    // block-counter comment above). Two state bytes evolve
                    // on every E2h, then the card performs a single-byte DMA
                    // write of valadd to memory on the 8-bit channel.
                    ident_valadd_ = uint8_t(ident_valadd_ + (params_[0] ^ ident_valxor_));
                    ident_valxor_ = uint8_t((ident_valxor_ >> 2) | (ident_valxor_ << 6));
                    buffer_[0] = ident_valadd_;
                    is_input_ = true;
                    bits16_ = false;
                    transfer_len_ = 1;
                    transfer_ready_ = true;
                    mode_ = Mode::kIdentify;
                    break;
                case 0xE1:  // major then minor (SBPG chapter 6)
                    read_fifo_.push_back(kDspMajor);
                    read_fifo_.push_back(kDspMinor);
                    break;
                case 0xE3:
                    for (const char *p = kCopyright; *p; ++p) read_fifo_.push_back(uint8_t(*p));
                    read_fifo_.push_back(0);  // the string is null-terminated
                    break;
                case 0xE4: test_reg_ = params_[0]; break;
                case 0xE8: read_fifo_.push_back(test_reg_); break;
                // Diagnostic interrupt triggers: this is how a driver's
                // auto-detection works out which IRQ line the card is really
                // jumpered/programmed to.
                case 0xF2: irq8_ = true; break;
                case 0xF3: irq16_ = true; break;
                default: break;  // unknown command: ignored, like real hardware
            }
            break;
    }
    params_needed_ = params_got_ = 0;
}

void SoundBlaster::begin_dma(bool input, bool is16, bool ai, bool stereo, bool signed_data,
                             uint32_t dma_units) {
    is_input_ = input;
    bits16_ = is16;
    autoinit_ = ai;
    stereo_ = stereo;
    signed_data_ = signed_data;
    // The DSP's block counter counts DMA transfer cycles, not audio frames --
    // bytes on the 8-bit channel, WORDS on the 16-bit one. SBPG's own Bxh
    // pages give the 16-bit length in words, which is only coherent if the
    // counter sits on the DMA side of the FIFO; the same counter then serves
    // stereo, so a stereo block covers half as many frames as it does units.
    // Reading the length as frames instead makes an 8-bit stereo block twice
    // too long, so a double-buffering driver's refill falls a half-block
    // behind the play position and every sound is heard twice -- which is
    // exactly how DOOM 1.2 (11025 Hz 8-bit stereo, C6h mode 20h, 2 KB
    // auto-init buffer) reproduced it. DOSBox-X's sblaster.cpp likewise
    // decrements its block counter by bytes read for 8-bit stereo and by
    // words for 16-bit; flagged as second-hand corroboration, not as the
    // primary source, per this repo's rule on other emulators.
    const uint32_t units_per_frame = stereo ? 2u : 1u;
    block_frames_ = block_left_ = dma_units / units_per_frame;
    adpcm_ = Adpcm::kNone;
    adpcm_need_ref_ = false;
    rate_hz_ = clamp_rate_for_format(rate_hz_, 0);
    exit_autoinit_ = false;
    paused_ = false;
    transfer_ready_ = false;
    transfer_len_ = 0;
    frame_credit_ = 0.0;
    mode_ = Mode::kDma;
}

void SoundBlaster::begin_adpcm(Adpcm format, bool ai, bool need_ref, uint32_t dma_bytes) {
    is_input_ = false;  // decompression is output-only (SBPG 3-7)
    bits16_ = stereo_ = signed_data_ = false;
    autoinit_ = ai;
    adpcm_ = format;
    adpcm_need_ref_ = need_ref;
    // A fresh decoder starts from silence-ish state; a reference block
    // overwrites both on its first byte.
    adpcm_ref_ = 0x80;
    adpcm_step_ = 0;
    // Unlike PCM, the DSP's length for these commands counts compressed
    // bytes, and one byte carries 2, 3 or 4 samples -- so the block counter
    // here is in bytes and advance()/finish_transfer() convert.
    block_frames_ = block_left_ = dma_bytes;
    const int bits = format == Adpcm::k4Bit ? 4 : (format == Adpcm::k3Bit ? 3 : 2);
    rate_hz_ = clamp_rate_for_format(rate_hz_, bits);
    exit_autoinit_ = false;
    paused_ = false;
    transfer_ready_ = false;
    transfer_len_ = 0;
    frame_credit_ = 0.0;
    mode_ = Mode::kDma;
}

void SoundBlaster::decode_adpcm_byte(uint8_t byte, uint64_t cycle, double cycles_per_frame) {
    // Code packing per ratio: 4-bit takes the high nibble first; 3-bit packs
    // three samples as bits 7-5, 4-2 and then bits 1-0 shifted up into the
    // same 3-bit code space (the "2.6-bit" mode's short last sample); 2-bit
    // takes four codes from the top down.
    uint8_t codes[4] = {};
    int n = frames_per_byte();
    const int8_t *scale = kAdpcm4Scale;
    const uint8_t *adjust = kAdpcm4Adjust;
    int entries = 64;
    switch (adpcm_) {
        case Adpcm::k4Bit:
            codes[0] = uint8_t(byte >> 4);
            codes[1] = uint8_t(byte & 0x0F);
            break;
        case Adpcm::k3Bit:
            codes[0] = uint8_t((byte >> 5) & 0x07);
            codes[1] = uint8_t((byte >> 2) & 0x07);
            codes[2] = uint8_t((byte & 0x03) << 1);
            scale = kAdpcm3Scale;
            adjust = kAdpcm3Adjust;
            entries = 40;
            break;
        case Adpcm::k2Bit:
            codes[0] = uint8_t((byte >> 6) & 0x03);
            codes[1] = uint8_t((byte >> 4) & 0x03);
            codes[2] = uint8_t((byte >> 2) & 0x03);
            codes[3] = uint8_t(byte & 0x03);
            scale = kAdpcm2Scale;
            adjust = kAdpcm2Adjust;
            entries = 24;
            break;
        default: return;
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t s = decode_adpcm(codes[i], scale, adjust, entries, adpcm_ref_, adpcm_step_);
        const int16_t v = expand8(s, false);  // decoded samples are unsigned 8-bit
        push_sample(cycle + uint64_t(double(i) * cycles_per_frame), v, v);
    }
}

void SoundBlaster::push_sample(uint64_t cycle, int16_t l, int16_t r) {
    if (samples_.size() >= kMaxSamples) samples_.pop_front();  // drop oldest -- see header
    samples_.push_back({cycle, l, r});
}

void SoundBlaster::raise_block_irq() {
    if (bits16_) irq16_ = true;
    else irq8_ = true;
}

void SoundBlaster::fill_input_buffer(std::size_t len) {
    // Nothing is plugged into the line/mic inputs, so a real card digitizes
    // silence -- which is not the same byte in both formats (see expand8/
    // expand16's note on where silence sits).
    const uint8_t quiet = (bits16_ || signed_data_) ? 0x00 : 0x80;
    std::fill(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(len), quiet);
}

void SoundBlaster::advance(uint64_t cpu_cycles, uint64_t delta) {
    frame_credit_ += double(delta);
    const double cycles_per_frame = cpu_hz_ / double(rate_hz_ ? rate_hz_ : 1);
    const uint64_t due = uint64_t(frame_credit_ / cycles_per_frame);
    if (due == 0) return;

    if (mode_ == Mode::kSilence) {
        uint32_t frames = uint32_t(std::min<uint64_t>(due, block_left_));
        frame_credit_ -= double(frames) * cycles_per_frame;
        const double span = double(frames) * cycles_per_frame;
        uint64_t start = double(cpu_cycles) > span ? cpu_cycles - uint64_t(span) : 0;
        for (uint32_t i = 0; i < frames; ++i)
            push_sample(start + uint64_t(double(i) * cycles_per_frame), 0, 0);
        block_left_ -= frames;
        if (block_left_ == 0) {
            raise_block_irq();
            mode_ = Mode::kIdle;
        }
        return;
    }

    // The chipset has not serviced the last burst yet; the DMA request stays
    // asserted rather than a second one stacking behind it.
    if (transfer_ready_) return;

    uint64_t burst_frames;  // frames this burst's bytes will actually cover
    if (adpcm_ != Adpcm::kNone) {
        // block_left_ counts compressed bytes here, so convert the frames
        // that came due into the bytes that carry them, rounding up: a byte
        // is indivisible. The credit consumed is for the frames those bytes
        // really produce, so rounding up borrows against the next burst
        // instead of drifting the long-run rate.
        const uint64_t fpb = uint64_t(frames_per_byte());
        uint32_t bytes = uint32_t(std::min<uint64_t>((due + fpb - 1) / fpb, block_left_));
        bytes = uint32_t(std::min<std::size_t>(bytes, kBufferBytes));
        if (bytes == 0) return;
        transfer_len_ = bytes;
        burst_frames = uint64_t(bytes) * fpb;
    } else {
        const std::size_t bpf = std::size_t(bytes_per_frame());
        uint32_t frames = uint32_t(std::min<uint64_t>(due, block_left_));
        frames = uint32_t(std::min<std::size_t>(frames, kBufferBytes / bpf));
        if (frames == 0) return;
        transfer_len_ = std::size_t(frames) * bpf;
        burst_frames = frames;
    }
    frame_credit_ -= double(burst_frames) * cycles_per_frame;
    // Stamp the burst against the real time it actually covered, which ended
    // at cpu_cycles -- so the front end sees samples at their true instants
    // even though the bytes move in one step (see the file header).
    const double span = double(burst_frames) * cycles_per_frame;
    xfer_start_cycle_ = double(cpu_cycles) > span ? cpu_cycles - uint64_t(span) : 0;
    if (is_input_) fill_input_buffer(transfer_len_);
    transfer_ready_ = true;
}

void SoundBlaster::finish_transfer(std::size_t actual_len) {
    transfer_ready_ = false;
    if (mode_ == Mode::kIdentify) {
        // The one-byte E2h write is not a DSP "block": no length/block-
        // counter accounting and no interrupt, unlike every other transfer
        // this device starts (see run_command's E2h case). A masked channel
        // moves nothing, and the card keeps the request asserted until the
        // driver unmasks it -- which is the normal order for the DMA probe
        // this command exists to serve.
        if (actual_len == 0) {
            transfer_ready_ = true;
            return;
        }
        transfer_len_ = 0;
        mode_ = Mode::kIdle;
        return;
    }

    if (adpcm_ != Adpcm::kNone) {
        const std::size_t bytes = std::min(actual_len, transfer_len_);
        transfer_len_ = 0;
        const double cycles_per_frame = cpu_hz_ / double(rate_hz_ ? rate_hz_ : 1);
        const int fpb = frames_per_byte();
        uint64_t cycle = xfer_start_cycle_;
        for (std::size_t i = 0; i < bytes; ++i) {
            if (adpcm_need_ref_) {
                // SBPG 3-7: "The first byte of the compressed data is always
                // a reference byte. It is not ADPCM code but an actual data
                // byte value." It seeds the predictor and produces no sample
                // of its own.
                adpcm_need_ref_ = false;
                adpcm_ref_ = buffer_[i];
                adpcm_step_ = 0;
                continue;
            }
            decode_adpcm_byte(buffer_[i], cycle, cycles_per_frame);
            cycle += uint64_t(double(fpb) * cycles_per_frame);
        }
        const uint32_t consumed_bytes = uint32_t(std::min<std::size_t>(bytes, block_left_));
        block_left_ -= consumed_bytes;
        if (block_left_ == 0) {
            raise_block_irq();
            // An auto-init ADPCM block re-reads the same buffer, but the DSP
            // consumed its reference byte once when the command started, so
            // the next pass decodes that first byte as compressed data.
            if (autoinit_ && !exit_autoinit_) block_left_ = block_frames_;
            else mode_ = Mode::kIdle;
        }
        return;
    }

    const std::size_t bpf = std::size_t(bytes_per_frame());
    const std::size_t frames = std::min(actual_len, transfer_len_) / bpf;
    transfer_len_ = 0;

    if (!is_input_) {
        const double cycles_per_frame = cpu_hz_ / double(rate_hz_ ? rate_hz_ : 1);
        for (std::size_t i = 0; i < frames; ++i) {
            const uint64_t cycle = xfer_start_cycle_ + uint64_t(double(i) * cycles_per_frame);
            const uint8_t *p = buffer_.data() + i * bpf;
            int16_t l, r;
            if (bits16_) {
                l = expand16(p[0], p[1], signed_data_);
                r = stereo_ ? expand16(p[2], p[3], signed_data_) : l;
            } else {
                l = expand8(p[0], signed_data_);
                r = stereo_ ? expand8(p[1], signed_data_) : l;
            }
            push_sample(cycle, l, r);
        }
    }

    const uint32_t consumed = uint32_t(std::min<std::size_t>(frames, block_left_));
    block_left_ -= consumed;
    if (block_left_ == 0) {
        // End of a programmed block: interrupt, then either reload for the
        // next auto-init pass or stop. SBPG: the exit commands D9h/DAh take
        // effect "at the end of the current block transfer", which is here.
        raise_block_irq();
        if (autoinit_ && !exit_autoinit_) block_left_ = block_frames_;
        else mode_ = Mode::kIdle;
    }
}

std::vector<SoundBlaster::Sample> SoundBlaster::drain_samples() {
    std::vector<Sample> out(samples_.begin(), samples_.end());
    samples_.clear();
    return out;
}



}  // namespace pc486
