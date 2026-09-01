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
constexpr uint8_t ST_RDA = 0x01;   // receive data available   (0 = a byte waits)
constexpr uint8_t ST_TBE = 0x80;   // transmit buffer empty    (0 = ready to record)
constexpr int     kRecQuiet = 16;  // status polls with no OUT 07 -> recording ended
}  // namespace

void CassetteACR::reset() {
    // a mounted tape survives a CPU reset — you don't lose the cassette — but the
    // transport stops and the head returns to the start
    pos_ = 0;
    mode_ = kIdle;
    idle_polls_ = 0;
}

void CassetteACR::mount(const uint8_t *data, std::size_t len) {
    tape_.assign(data, data + len);
    pos_ = 0;
    mode_ = kIdle;
    dirty_ = false;
    idle_polls_ = 0;
}

void CassetteACR::eject() {
    tape_.clear();
    pos_ = 0;
    mode_ = kIdle;
    dirty_ = false;
}

uint8_t CassetteACR::in(uint8_t port) {
    if (port == 0x07) {                        // read the next byte off the tape
        mode_ = kPlaying;
        ++io_ticks_;
        return pos_ < tape_.size() ? tape_[pos_++] : 0;
    }
    if (port == 0x06) {                        // status
        // A recording that has gone quiet means the deck moved on to something
        // else (typically CLOAD right after CSAVE): rewind for playback so the
        // round-trip works without the user reaching for the transport.
        if (mode_ == kRecording && ++idle_polls_ > kRecQuiet) {
            mode_ = kIdle;
            pos_ = 0;
            idle_polls_ = 0;
        }
        uint8_t s = ST_RDA;                    // bit 7 = 0: always ready to record
        const bool canRead = (mode_ != kRecording) && (pos_ < tape_.size());
        if (canRead) s &= ~ST_RDA;             // bit 0 = 0: a byte is waiting
        return s;
    }
    return 0xFF;
}

void CassetteACR::out(uint8_t port, uint8_t value) {
    if (port == 0x07) {                        // write a byte onto the tape
        if (mode_ != kRecording) {             // first byte of a save: from the top
            mode_ = kRecording;
            pos_ = 0;
            tape_.clear();
        }
        tape_.push_back(value);
        pos_ = tape_.size();
        dirty_ = true;
        idle_polls_ = 0;
        ++io_ticks_;
        return;
    }
    // OUT 0x06 is ACIA / motor-relay control; the motor isn't modelled.
}

}  // namespace altair
