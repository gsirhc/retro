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

// SBPG chapter 3, "Digitized Sound I/O Transfer Rate":
//   Time Constant = 65536 - (256 000 000 / (channels * sampling rate))
// with only the high byte programmed through command 40h, which reduces to
// tc = 256 - (1 000 000 / (channels * rate)). The driver is the one that
// folds `channels` in before writing the byte (Allegro's sb.c writes
// `256 - 1000000/rate` outright), so inverting it here gives back the rate
// the DSP clocks samples at.
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
    reset_dsp(false);
    reset_mixer();
    test_reg_ = 0;  // cleared only by a cold power-on; a DSP reset leaves it alone
    samples_.clear();
    frame_credit_ = 0.0;
    prev_cycles_ = 0;
    reset_asserted_ = false;
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
    return five_bit_gain(mixer_[0x30]) * five_bit_gain(mixer_[0x32]);
}
float SoundBlaster::output_gain_right() const {
    return five_bit_gain(mixer_[0x31]) * five_bit_gain(mixer_[0x33]);
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
    switch (port - base_) {
        // FM (OPL3) status registers. 00h, not a plausible OPL status byte:
        // AdLib detection is meant to fail here, because no FM synthesizer
        // is implemented -- see the file header.
        case 0x00: case 0x02: case 0x08: return 0x00;
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
        default: return 0xFF;  // write-only or reserved port in the block: open bus
    }
}

void SoundBlaster::out(uint16_t port, uint8_t v) {
    switch (port - base_) {
        // FM register/data ports: decoded and accepted, but no FM synthesis
        // (see the file header).
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: return;
        case 0x04: mixer_index_ = v; return;
        case 0x05: mixer_write(mixer_index_, v); return;
        case 0x06:
            // SBPG 2-2: write a 1, wait ~3us, write a 0; the DSP then posts
            // 0AAh. Only the 1 -> 0 transition completes a reset, which is
            // why a driver that leaves the card parked in reset (writing 1
            // and stopping) gets no acknowledge byte.
            if (v & 0x01) {
                reset_asserted_ = true;
            } else if (reset_asserted_) {
                reset_asserted_ = false;
                reset_dsp(true);
            }
            return;
        case 0x0C: dsp_write(v); return;
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

        // Legacy 8-bit mono unsigned PCM, always paced by the 40h time
        // constant (SBPG 3-13 and 3-15). wLength/wBlkSize are "one less than
        // the actual number of bytes", hence the +1.
        case 0x14: begin_dma(false, false, false, false, false, uint32_t(len16) + 1); break;
        case 0x24: begin_dma(true, false, false, false, false, uint32_t(len16) + 1); break;
        case 0x1C: begin_dma(false, false, true, false, false, uint32_t(dsp_block_size_) + 1); break;
        case 0x2C: begin_dma(true, false, true, false, false, uint32_t(dsp_block_size_) + 1); break;

        // ADPCM: parameters swallowed so the command stream stays in sync,
        // no playback started (see the file header).
        case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x1F: case 0x7D: case 0x7F:
            break;

        // High-speed mode does not exist on DSP 4.xx (SBPG chapter 6's
        // availability matrix lists 90h/91h/98h/99h for 2.01+ and 3.xx only),
        // so a real SB16 ignores these exactly as this does.
        case 0x90: case 0x91: case 0x98: case 0x99: break;
        // A0h/A8h (mono/stereo input select) likewise "no longer exists on
        // DSP version 4.xx"; stereo is selected by the Bxh/Cxh mode byte.
        case 0xA0: case 0xA8: break;

        case 0x40: rate_hz_ = time_constant_to_rate(params_[0]); break;
        // 41h/42h carry the true sampling rate in Hz, HIGH byte first --
        // the opposite order from every length parameter on the card.
        case 0x41: case 0x42:
            rate_hz_ = uint32_t(uint32_t(params_[0]) << 8 | params_[1]);
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
    exit_autoinit_ = false;
    paused_ = false;
    transfer_ready_ = false;
    transfer_len_ = 0;
    frame_credit_ = 0.0;
    mode_ = Mode::kDma;
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
    const double cycles_per_frame = kCpuHz / double(rate_hz_ ? rate_hz_ : 1);
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

    const std::size_t bpf = std::size_t(bytes_per_frame());
    uint32_t frames = uint32_t(std::min<uint64_t>(due, block_left_));
    frames = uint32_t(std::min<std::size_t>(frames, kBufferBytes / bpf));
    if (frames == 0) return;
    frame_credit_ -= double(frames) * cycles_per_frame;
    transfer_len_ = std::size_t(frames) * bpf;
    // Stamp the burst against the real time it actually covered, which ended
    // at cpu_cycles -- so the front end sees samples at their true instants
    // even though the bytes move in one step (see the file header).
    const double span = double(frames) * cycles_per_frame;
    xfer_start_cycle_ = double(cpu_cycles) > span ? cpu_cycles - uint64_t(span) : 0;
    if (is_input_) fill_input_buffer(transfer_len_);
    transfer_ready_ = true;
}

void SoundBlaster::finish_transfer(std::size_t actual_len) {
    transfer_ready_ = false;
    const std::size_t bpf = std::size_t(bytes_per_frame());
    const std::size_t frames = std::min(actual_len, transfer_len_) / bpf;
    transfer_len_ = 0;

    if (!is_input_) {
        const double cycles_per_frame = kCpuHz / double(rate_hz_ ? rate_hz_ : 1);
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
