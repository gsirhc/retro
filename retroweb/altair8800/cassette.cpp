// MITS 88-ACR, see cassette.h.
// Status is active-low, per our 8K BASIC 4.0 ROM: CLOAD waits while bit 0 is set
// (IN 06 / ANI 01), CSAVE waits while bit 7 is set (IN 06 / ANI 80).

#include "cassette.h"

namespace altair {

namespace {
constexpr uint8_t     ST_RDA = 0x01;   // receive data available   (0 = a byte waits)
constexpr uint8_t     ST_TBE = 0x80;   // transmit buffer empty    (0 = ready to record)
constexpr std::size_t kGapBytes = 12;  // blank run so CLOAD can resync onto the next program
}  // namespace

void CassetteACR::reset() {
    // a mounted tape survives reset; the transport stops and the head rewinds
    pos_ = 0;
    head_frac_ = 0;
    credit_ = 0;
    mode_ = kIdle;
    wind_ = 0;
    prev_tick_cy_ = 0;
}

void CassetteACR::mount(const uint8_t *data, std::size_t len) {
    tape_.assign(data, data + len);
    pos_ = 0;
    head_frac_ = 0;
    credit_ = 0;
    mode_ = kIdle;
    wind_ = 0;
    dirty_ = false;
    mounted_ = true;
}

void CassetteACR::eject() {
    tape_.clear();
    pos_ = 0;
    head_frac_ = 0;
    credit_ = 0;
    mode_ = kIdle;
    wind_ = 0;
    dirty_ = false;
    mounted_ = false;
}

void CassetteACR::setWind(int dir) {
    head_frac_ = 0;
    if (cycles_per_byte_ == 0) {          // "Max": the seek is instant
        if (dir > 0) pos_ = capacity();
        else if (dir < 0) pos_ = 0;
        wind_ = 0;
        return;
    }
    wind_ = dir < 0 ? -1 : dir > 0 ? 1 : 0;
}

void CassetteACR::tick(uint64_t cpuCycles) {
    // Front-panel RESET zeroes cpu_.cycles, which feeds this clock; the deck is
    // not on the bus and keeps rolling. Treat a backward jump as no time passed.
    if (cpuCycles < prev_tick_cy_) {
        prev_tick_cy_ = cpuCycles;
        cpu_cycles_ = cpuCycles;
        return;
    }
    uint64_t d = cpuCycles - prev_tick_cy_;
    prev_tick_cy_ = cpuCycles;
    cpu_cycles_ = cpuCycles;
    if (cycles_per_byte_ == 0) { credit_ = 1e9; return; }   // "Max": reads never gated

    // FF / REW wind the head fast, whether or not the CPU touches the board
    if (wind_) {
        head_frac_ += static_cast<double>(d) / cycles_per_byte_ * kWindMult * (wind_ > 0 ? 1 : -1);
        long step = static_cast<long>(head_frac_);
        if (step != 0) {
            head_frac_ -= step;
            long np = static_cast<long>(pos_) + step;
            if (np <= 0)                        { pos_ = 0;          wind_ = 0; }   // hit the start
            else if (static_cast<std::size_t>(np) >= capacity()) { pos_ = capacity(); wind_ = 0; }  // hit the end
            else                               pos_ = static_cast<std::size_t>(np);
        }
        return;
    }

    if (!motor_) return;

    // credit_ counts byte-times gone by that the CPU hasn't read or written yet
    const double cap = std::max(8.0, byteRate() * 0.25);
    credit_ += static_cast<double>(d) / cycles_per_byte_;

    if (mode_ == kRecording) {
        if (credit_ > cap) credit_ = cap;
        return;
    }

    // playback: credit past the cap means nothing is reading; surplus rolls past the head
    if (credit_ > cap) {
        head_frac_ += credit_ - cap;      // carry the sub-byte remainder
        credit_ = cap;
        long spill = static_cast<long>(head_frac_);
        if (spill > 0) {
            head_frac_ -= spill;
            long np = static_cast<long>(pos_) + spill;
            if (static_cast<std::size_t>(np) >= capacity()) {
                pos_ = capacity();
                motor_ = false;
                if (mode_ == kPlaying) mode_ = kIdle;   // PLAY ran off the end -> auto-stop
            } else {
                pos_ = static_cast<std::size_t>(np);
            }
        }
    }
}

uint8_t CassetteACR::in(uint8_t port) {
    if (port == 0x07) {                        // read the next byte off the tape
        if (!motor_) return 0;                 // transport stopped: nothing feeds
        mode_ = kPlaying;
        if (credit_ >= 1) credit_ -= 1;        // consumed a byte-time
        ++io_ticks_;
        return pos_ < tape_.size() ? tape_[pos_++] : 0;
    }
    if (port == 0x06) {                        // status
        uint8_t s = ST_RDA | ST_TBE;           // nothing ready yet
        if (rec_armed_ && motor_ && ready())               // PLAY + REC: ready to take a byte
            s &= ~ST_TBE;                                  // bit 7 = 0
        if (mode_ != kRecording && motor_ && ready() && pos_ < tape_.size())
            s &= ~ST_RDA;                                  // bit 0 = 0: a byte is under the head
        return s;
    }
    return 0xFF;
}

void CassetteACR::out(uint8_t port, uint8_t value) {
    if (port == 0x07) {                        // write a byte onto the tape
        if (!rec_armed_ || !motor_) return;    // need PLAY + REC, like the real interlock
        if (mode_ != kRecording && pos_ == tape_.size() && pos_ != 0) {
            // appending after an earlier program -- leave a blank gap first
            tape_.resize(tape_.size() + kGapBytes, 0);
            pos_ = tape_.size();
        }
        mode_ = kRecording;
        if (credit_ >= 1) credit_ -= 1;
        // record at the head; the old tail stays on tape, so CLOAD stops at the new end
        if (pos_ < tape_.size()) {
            tape_[pos_] = value;
        } else {
            if (pos_ > tape_.size()) tape_.resize(pos_, 0);
            tape_.push_back(value);
        }
        ++pos_;
        dirty_ = true;
        ++io_ticks_;
        return;
    }
    // OUT 0x06 (motor relay) is not modelled.
}

}  // namespace altair
