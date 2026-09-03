// MITS 88-ACR — see cassette.h.
//
// Status polarity, read straight out of our 8K BASIC 4.0 ROM:
//   CLOAD byte:  IN 06 / ANI 01 / JNZ <back>   -> wait while bit 0 is SET,
//                                                 proceed (IN 07) when it CLEARS
//   CSAVE byte:  IN 06 / ANI 80 / JNZ <back>   -> wait while bit 7 is SET,
//                                                 proceed (OUT 07) when it CLEARS
// So both "ready" conditions are active-low: bit 0 low = a byte is available to
// read, bit 7 low = the recorder will take another byte.

#include "cassette.h"

namespace altair {

namespace {
constexpr uint8_t     ST_RDA = 0x01;   // receive data available   (0 = a byte waits)
constexpr uint8_t     ST_TBE = 0x80;   // transmit buffer empty    (0 = ready to record)
constexpr std::size_t kGapBytes = 12;  // blank run between programs, so CLOAD can
                                       // resync onto the next one (the tape gap
                                       // you get pressing STOP then REC again)
}  // namespace

void CassetteACR::reset() {
    // a mounted tape survives a CPU reset — you don't lose the cassette — but the
    // transport stops and the head returns to the start
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

    // the tape rolls forward at the selected rate; credit_ is how many byte-times
    // have gone by that the CPU hasn't read (or written) yet -- BASIC drains it
    // as fast as its CLOAD / CSAVE loop runs, so 25x / 50x really are that fast
    const double cap = std::max(8.0, byteRate() * 0.25);
    credit_ += static_cast<double>(d) / cycles_per_byte_;

    // while recording, the head advances on each OUT 0x07, not on its own
    if (mode_ == kRecording) {
        if (credit_ > cap) credit_ = cap;
        return;
    }

    // playback: any credit past the cap means nothing is keeping up (PLAY with
    // no CLOAD, or a slow reader) -- the surplus bytes roll past the head unread
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
        // record AT THE HEAD: overwrite in place, or extend the tape. The old
        // tail past the new bytes stays on the tape, as it would physically --
        // CLOAD stops at the new program's end.
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
    // OUT 0x06 is ACIA / motor-relay control; the motor isn't modelled.
}

}  // namespace altair
