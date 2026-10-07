// MITS Altair 88-ACR audio-cassette interface (Kansas City standard, 300 baud).
// Two I/O ports: 0x06 status/control, 0x07 data. No audio model: the tape is a
// byte buffer with a head at pos(), so recording overwrites at the head.
//
// credit_ counts byte-times the tape has moved that the CPU hasn't consumed.
// PLAY adds credit at the selected rate, IN/OUT 0x07 spend it, and any surplus
// rolls the head forward. FF/REW wind at kWindMult times the rate.
// setSpeed(0) is "Max": reads are never gated.
// Status polarity is checked against our 8K BASIC ROM (see cassette.cpp).

#ifndef EMULATOR8080_CASSETTE_H
#define EMULATOR8080_CASSETTE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace altair {

class CassetteACR {
public:
    CassetteACR() { reset(); }

    bool owns(uint8_t port) const { return port == 0x06 || port == 0x07; }
    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    void reset();

    void setMotor(bool on) {
        motor_ = on;
        if (on) wind_ = 0;                       // PLAY releases FF / REW
        else if (mode_ == kPlaying) mode_ = kIdle;
    }
    void setRecordArm(bool on) {
        rec_armed_ = on;
        if (!on && mode_ == kRecording) mode_ = kIdle;
    }
    // +1 = FAST-FORWARD, -1 = REW, 0 = release; instant at unlimited speed
    void setWind(int dir);
    bool motor() const { return motor_; }
    int  wind()  const { return wind_; }

    void tick(uint64_t cpuCycles);
    // Bytes per second; 0 = unlimited, against a 2 MHz CPU. Drops credit on change.
    void setSpeed(int bytesPerSec) {
        cycles_per_byte_ = bytesPerSec > 0 ? (2000000ull / bytesPerSec) : 0;
        credit_ = 0;
    }

    // ---- host / front-end side --------------------------------------
    void mount(const uint8_t *data, std::size_t len);   // insert a tape, rewound
    void eject();
    void rewind() { pos_ = 0; head_frac_ = 0; credit_ = 0; }
    bool loaded() const { return mounted_; }     // a blank tape counts as loaded

    const std::vector<uint8_t> &data() const { return tape_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    enum Mode { kIdle, kPlaying, kRecording };
    // Which transport key is down: 0 stop, 1 play, 2 rec, 3 FF, 4 REW.
    enum Transport { kStop, kPlay, kRec, kFwd, kRewind };
    Mode        mode()  const { return mode_; }
    int         transport() const {
        if (motor_) return rec_armed_ ? kRec : kPlay;
        if (wind_ > 0) return kFwd;
        if (wind_ < 0) return kRewind;
        return kStop;
    }
    std::size_t pos()   const { return pos_; }
    std::size_t len()   const { return tape_.size(); }
    std::size_t capacity() const {
        return std::max<std::size_t>(tape_.size() + kTapeSlack, kMinTape);
    }
    uint64_t    ioTicks() const { return io_ticks_; }

private:
    static constexpr std::size_t kMinTape   = 16384;  // a blank tape's usable length
    static constexpr std::size_t kTapeSlack  = 4096;  // blank room past a recording
    static constexpr int         kWindMult  = 30;     // FF / REW travel vs. play rate

    double byteRate() const {
        return cycles_per_byte_ ? 2000000.0 / cycles_per_byte_ : 0.0;
    }
    bool ready() const { return cycles_per_byte_ == 0 || credit_ >= 1.0; }

    std::vector<uint8_t> tape_;
    std::size_t pos_       = 0;
    double      head_frac_ = 0;     // sub-byte head travel carried between ticks (wind)
    double      credit_    = 0;     // byte-times gone by that the CPU hasn't read yet
    Mode        mode_      = kIdle;
    bool        dirty_     = false;
    uint64_t    io_ticks_  = 0;

    bool     mounted_         = false;  // a cassette (blank or not) is in the deck
    bool     motor_           = false;  // playback transport engaged? (PLAY)
    bool     rec_armed_       = false;  // recording armed? (REC)
    int      wind_            = 0;      // -1 REW, 0, +1 FF

    uint64_t cpu_cycles_      = 0;
    uint64_t prev_tick_cy_    = 0;      // cpu_cycles_ at the last tick()
    uint64_t cycles_per_byte_ = 0;   // 0 = unlimited
};

} // namespace altair

#endif // EMULATOR8080_CASSETTE_H
