// Creative Sound Blaster 16 (CT1745 mixer + DSP version 4.05), ISA wiring:
// base I/O 0x220 (the factory default for every Sound Blaster card), the full
// 16-port block 0x220-0x22F, IRQ5, 8-bit DMA channel 1 and 16-bit DMA channel
// 5 -- i.e. exactly the card a period driver describes as
// `SET BLASTER=A220 I5 D1 H5 T6`, which is what this machine's DOS software
// finds in its environment.
//
// Everything here is programmed the way Creative's own "Sound Blaster Series
// Hardware Programming Guide" (Creative Technology Ltd, 1994 -- cited below
// as "SBPG" with its section names) says to program it. The port block, per
// SBPG Appendix A and Table 2-1 "DSP I/O Ports":
//
//   base+0h/1h  FM (OPL3) left status/address, data        -- see "no FM" below
//   base+2h/3h  FM (OPL3) right status/address, data       -- see "no FM" below
//   base+4h     Mixer register address (CT1745)
//   base+5h     Mixer data
//   base+6h     DSP reset                        (write only)
//   base+8h/9h  FM status/register, data (AdLib-compatible pair)
//   base+Ah     DSP read data                    (read only)
//   base+Ch     DSP write command/data (write) / write-buffer status (read)
//   base+Eh     DSP read-buffer status (read only); reading it also
//               acknowledges the 8-bit DMA-mode interrupt
//   base+Fh     16-bit DMA-mode interrupt acknowledge (read only, DSP 4.xx)
//
// The DSP reset handshake, the interrupt-acknowledge ports, and the
// software-selectable IRQ/DMA registers are all straight out of SBPG chapter
// 2 ("Introduction to DSP Programming"): write 1 to base+6h, write 0, then
// poll base+Eh bit 7 and read 0AAh from base+Ah; acknowledge 8-bit DMA
// interrupts by reading base+Eh and 16-bit ones by reading base+Fh; mixer
// register 82h reports which of the shared interrupt sources fired, mixer
// 80h selects the IRQ line (bit 1 = IRQ5, this card's default) and mixer 81h
// the DMA channels (bit 1 = DMA1, bit 5 = DMA5, likewise the defaults).
//
// Why 4.05 specifically: it's a real shipped SB16 DSP revision, and the
// version number is load-bearing rather than cosmetic. The DOS driver BOOM
// actually links (Allegro 3.x's `sb.c`) reads command E1h as
// `(major << 8) | minor` and branches on `>= 0x400` to pick its SB16 path --
// 41h to set a true sampling rate in Hz, then B6h with mode 30h (16-bit
// signed stereo) and a sample count for auto-init 16-bit playback on the
// 16-bit DMA channel. Report anything lower and the same driver silently
// drops to the SB-Pro/SB-2 path instead.
//
// Command coverage: the digitized-sound-output set real period software
// uses, which is what this milestone is for. Direct-mode 10h; legacy 8-bit
// single-cycle 14h and auto-init 1Ch with 40h/48h (SBPG 3-13..3-16); the
// DSP 4.xx Cxh/Bxh programmed 8-/16-bit, mono/stereo, signed/unsigned
// single-cycle and auto-init transfers with 41h/42h sampling rates (SBPG
// 3-26..3-29 and the Bxh/Cxh command pages, which is where the bCommand
// A/D-vs-D/A, auto-init and FIFO bits and the bMode stereo/signed bits come
// from); the pause/continue/exit controls D0h/D4h/D5h/D6h/D9h/DAh; speaker
// D1h/D3h/D8h; the 80h silence period; and the identification/diagnostic
// commands E0h, E1h, E3h, E4h, E8h, F2h and F3h a driver uses to find the
// card and work out which IRQ it is really on.
//
// Scope/simplifications (see PC486_REVIEW.md):
//  - No FM synthesis. base+0h..3h and base+8h..9h are decoded (a real card
//    decodes its whole 16-port block) and accept writes, but the FM status
//    registers read back 00h, so the standard AdLib detection sequence
//    (reset both timers, read status, start timer 1, read status again and
//    expect C0h) fails -- deliberately. A fake OPL3 that passed detection
//    and then produced silence would be worse than absent hardware: software
//    would believe its music is playing. 0x388/0x389, the alternate FM
//    address pair, are for the same reason not decoded at all. FM is the
//    stretch goal this milestone explicitly left out.
//  - No MIDI (SB-MIDI 30h/31h/34h-38h or MPU-401 at 0x330), no joystick
//    port, no ADPCM. The ADPCM playback commands (16h/17h/74h-77h, 1Fh/7Dh/
//    7Fh) are decoded far enough to swallow their parameter bytes -- so a
//    driver that probes one doesn't desynchronize the command stream -- but
//    start no playback.
//  - The high-speed commands 90h/91h/98h/99h are accepted and ignored,
//    because SBPG's own availability matrix lists them for DSP 2.01+ and
//    3.xx only, not 4.xx: DSP 4.xx has no high-speed mode (it reaches full
//    rate in normal mode). Allegro only issues them after detecting a
//    version below 4.00, so this is the real card's behavior, not a gap.
//  - The write-buffer status at base+Ch always reports "ready" (7Fh). Real
//    hardware is briefly busy after each byte and a handful of demoscene
//    programs time that; ordinary drivers poll the bit, which works either
//    way.
//  - Recording (A/D: 24h/2Ch, C8h/CEh, B8h/BEh) is accepted and paced, and
//    delivers digital silence, which is what a real card with nothing
//    plugged into its line/mic inputs delivers. Its interrupts fire on
//    schedule so a recording program completes instead of hanging.
//  - Playback pacing is the same accumulated-credit scheme fdc765.h uses:
//    real elapsed wall-clock time per sample is exact (never sped up, per
//    CLAUDE.md), but the bytes for a batch of samples move in one burst
//    rather than one DMA cycle per sample, so the DMA address/count
//    registers step in bursts. Software that waits for the block interrupt
//    (universal practice) cannot tell; software polling the DMA count to
//    find the play position mid-block would see it move in steps.
//
// --- what the chipset has to wire up ------------------------------------
// This device never reaches into system memory or the DMA controller
// itself, exactly like fdc765.h: `chipset.cpp` orchestrates the byte move
// via transfer_ready()/transfer_length()/transfer_buffer()/
// finish_transfer(), and raises the IRQ line irq_line() names on the 0->1
// edge of irq_pending(). Two details differ from the floppy's channel-2
// case and matter for getting the wiring right:
//
//  - A 16-bit transfer (transfer_is_16bit()) runs on DMA2's channel 5, and
//    DMA2's address and count registers count 16-bit WORDS, not bytes: the
//    8237's address outputs drive A1-A16 and the page register supplies
//    A17-A23. So the physical address is
//        (uint32_t(dma2.page(1)) << 16) | (uint32_t(dma2.address(1)) << 1)
//    (channel 5 is DMA2's index 1), the available byte count is
//    (count + 1) * 2, and each byte pair moved is one dma2.advance(1).
//    Page-register bit 0 is not connected on the 16-bit channels.
//  - Auto-init playback (transfer_is_autoinit()) expects the DMA controller
//    to reload address and count from its base registers at terminal count,
//    which `Dma8237` models directly (its Channel carries the 8237A-5's own
//    base_address/base_count shadow pair). The device keeps asking for bytes
//    until the driver sends the exit command DAh/D9h, which is the whole
//    point of auto-init mode.
//  - Direction follows the DSP command's A/D bit, which is the opposite
//    sense from the floppy's write-to-disk flag: playback (the common case,
//    transfer_is_input() false) reads memory *into* transfer_buffer(), and
//    only recording writes the buffer out to memory. See PC486_REVIEW.md §13.
#ifndef PC486_SOUNDBLASTER_H
#define PC486_SOUNDBLASTER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc486 {

class SoundBlaster {
public:
    // The factory default base I/O address for every Sound Blaster card
    // (SBPG Appendix A) and the period-standard SB16 jumper/driver defaults
    // that go with it.
    static constexpr uint16_t kDefaultBase = 0x220;
    static constexpr int kDefaultIrq = 5;
    static constexpr int kDefaultDma8 = 1;
    static constexpr int kDefaultDma16 = 5;
    // A real shipped SB16 DSP revision -- see the file header on why the
    // exact value is load-bearing for Allegro's card detection.
    static constexpr uint8_t kDspMajor = 4;
    static constexpr uint8_t kDspMinor = 5;

    explicit SoundBlaster(uint16_t base = kDefaultBase) : base_(base) { reset(); }

    // Cold power-on: DSP idle, mixer back to the CT1745 defaults SBPG
    // chapter 4 lists, IRQ/DMA selection back to IRQ5/DMA1/DMA5.
    void reset();

    // --- chipset port decode ---------------------------------------------
    // The whole 16-port block, as a real card decodes it. 0x388/0x389 are
    // deliberately not claimed (see the file header's "no FM" note).
    bool owns(uint16_t port) const { return port >= base_ && port <= uint16_t(base_ + 0x0F); }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advances playback/record pacing against the CPU's running cycle
    // count, like Fdc765::tick. Inline with an early-out because
    // Machine::run_cycles() calls it after every instruction and a silent
    // card -- the common case -- must cost almost nothing (see
    // PC486_REVIEW.md §8).
    void tick(uint64_t cpu_cycles) {
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (mode_ == Mode::kIdle || paused_) return;
        advance(cpu_cycles, delta);
    }

    // --- interrupts -------------------------------------------------------
    // One IRQ line carries the 8-bit DMA, 16-bit DMA and MIDI sources; mixer
    // register 82h says which fired, and software acknowledges by reading
    // base+Eh (8-bit) or base+Fh (16-bit). SBPG 2-5.
    bool irq_pending() const { return (irq8_ || irq16_); }
    void clear_irq() { irq8_ = irq16_ = false; }  // real software acks via the ports above
    bool irq8_pending() const { return irq8_; }
    bool irq16_pending() const { return irq16_; }
    // Software-selected, from mixer register 80h; 5 out of reset.
    int irq_line() const;

    // --- DMA channel selection (mixer register 81h) -----------------------
    int dma_channel_8bit() const;   // 1 out of reset
    int dma_channel_16bit() const;  // 5 out of reset

    // --- chipset/DMA transfer handoff (see the file header) ---------------
    bool transfer_ready() const { return transfer_ready_; }
    // true = A/D, device -> memory (recording); false = D/A playback.
    bool transfer_is_input() const { return is_input_; }
    // true = the transfer runs on the 16-bit channel, whose registers count
    // words rather than bytes -- see the file header's address formula.
    bool transfer_is_16bit() const { return bits16_; }
    bool transfer_is_autoinit() const { return autoinit_; }
    int transfer_dma_channel() const { return bits16_ ? dma_channel_16bit() : dma_channel_8bit(); }
    std::size_t transfer_length() const { return transfer_len_; }   // bytes
    uint8_t *transfer_buffer() { return buffer_.data(); }
    // `actual_len` is however many bytes the chipset really moved -- fewer
    // than transfer_length() when the DMA channel's own count ran out
    // first, which is exactly the real terminal-count case.
    void finish_transfer(std::size_t actual_len);

    // --- audio output to the front end -----------------------------------
    // Same philosophy as PcSpeaker's edge log, one level up: this device
    // does not synthesize audio, it records what the DAC actually latched
    // and when. Each sample carries the CPU cycle at which it was clocked
    // out, so a Web Audio renderer can resample from real time rather than
    // this core committing to some particular output rate -- and a program
    // that changes sampling rate mid-stream (or plays 8-bit mono after
    // 16-bit stereo) needs no special case. Values are normalized to signed
    // 16-bit stereo, the card's own widest native format: 8-bit unsigned
    // data is centered and scaled up on the way in, mono is duplicated to
    // both channels.
    struct Sample {
        uint64_t cpu_cycle;
        int16_t left, right;
    };
    std::vector<Sample> drain_samples();

    // What the DAC is currently doing, for a front end that wants to show
    // it (and for tests).
    uint32_t sample_rate_hz() const { return rate_hz_; }
    bool playing() const { return mode_ != Mode::kIdle && !paused_; }
    bool stereo() const { return stereo_; }
    bool sixteen_bit() const { return bits16_; }
    bool speaker_on() const { return speaker_on_; }

    // The analog chain the samples above still have to pass through: the
    // CT1745's Voice and Master attenuators, as linear gains (SBPG chapter
    // 4: 5-bit fields, 0-31 => -62 dB to 0 dB in 2 dB steps, default 24).
    // Kept out of the sample path deliberately, so drain_samples() returns
    // exactly the values the program wrote.
    float output_gain_left() const;
    float output_gain_right() const;
    uint8_t mixer_register(uint8_t index) const { return mixer_[index]; }

private:
    enum class Mode { kIdle, kDma, kSilence };

    // 256 KB: comfortably more than the largest block a period driver
    // programs (the 8237 can only address 64 KB of bytes / 64 K words per
    // page anyway), so one transfer never needs splitting for buffer size.
    static constexpr std::size_t kBufferBytes = 256u * 1024u;
    // Same bound, and the same reason, as PcSpeaker::kMaxEdges: real
    // hardware has no such limit, this only caps memory if the front end
    // stops draining a card that is actively playing.
    static constexpr std::size_t kMaxSamples = 1u << 16;
    // This machine's real 66 MHz clock (machine.h's kCpuHz) -- tick() is fed
    // a counter paced at that rate, so this must match or playback runs at
    // the wrong wall-clock speed.
    static constexpr double kCpuHz = 66000000.0;

    void advance(uint64_t cpu_cycles, uint64_t delta);
    void dsp_write(uint8_t v);
    void run_command();
    void reset_dsp(bool from_reset_port);
    void reset_mixer();
    void mixer_write(uint8_t index, uint8_t v);
    uint8_t mixer_read(uint8_t index) const;
    // `dma_units` is the length exactly as the DSP command programmed it
    // (already +1'd): bytes on the 8-bit channel, words on the 16-bit one.
    // begin_dma converts to frames -- see its comment.
    void begin_dma(bool input, bool is16, bool ai, bool stereo, bool signed_data,
                   uint32_t dma_units);
    void push_sample(uint64_t cycle, int16_t l, int16_t r);
    void raise_block_irq();
    void fill_input_buffer(std::size_t len);
    int bytes_per_frame() const { return (bits16_ ? 2 : 1) * (stereo_ ? 2 : 1); }

    uint16_t base_;

    // DSP command sequencing.
    uint8_t cmd_ = 0;
    uint8_t params_[4] = {};
    int params_needed_ = 0, params_got_ = 0;
    std::deque<uint8_t> read_fifo_;
    uint8_t last_read_ = 0;   // real hardware re-reads the last byte from a drained FIFO
    uint8_t test_reg_ = 0;    // E4h/E8h diagnostic register; survives a DSP reset
    bool reset_asserted_ = false;
    bool speaker_on_ = false;

    // Transfer state.
    Mode mode_ = Mode::kIdle;
    bool is_input_ = false, bits16_ = false, autoinit_ = false, stereo_ = false;
    bool signed_data_ = false;
    bool paused_ = false;
    bool exit_autoinit_ = false;
    uint32_t rate_hz_ = 11025;      // no meaningful default on real hardware; a
                                    // driver always sets 40h or 41h before playing
    uint32_t block_frames_ = 0;     // 48h / Bxh-Cxh length, in frames
    uint32_t block_left_ = 0;
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
