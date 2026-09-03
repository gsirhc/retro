// MITS Altair 88-ACR audio-cassette interface.
//
// The 88-ACR records/plays the Kansas City standard onto an ordinary audio
// cassette at 300 baud. To the CPU it looks like a serial port on two I/O
// addresses (octal 6/7 = 0x06/0x07):
//
//   IN  0x06  status   (a data byte waiting? ready to record another?)
//   OUT 0x06  control  (motor relay etc. — ignored here)
//   IN  0x07  read the next byte off the tape
//   OUT 0x07  write a byte onto the tape
//
// Altair 4K/8K BASIC's CSAVE / CLOAD drive this board. There is no audio model:
// the "tape" is a byte buffer with a play/record head at pos(). It behaves like
// a real linear cassette — the head stays where you leave it, you can hold
// several programs on one tape and use REW / FAST-FORWARD to move between them,
// and recording overwrites at the head rather than wiping the tape.
//
// The transport runs on the CPU cycle clock (fed in via tick()). `credit_` is a
// running count of byte-times the tape has moved that the CPU hasn't consumed:
//   PLAY  — tick() adds credit at the selected byte rate; IN/OUT 0x07 spend it,
//           so BASIC can pull several bytes per frame at 25x/50x. If credit
//           overflows (nothing reading, or a slow reader) the surplus rolls the
//           head forward and those bytes are lost — the tape just plays.
//   FF/REW — wind the head at kWindMult times the rate; auto-stop at the ends.
//   setSpeed(0) — "Max": reads are never gated (credit is effectively infinite).
//
// Status-bit polarity is confirmed against our 8K BASIC ROM (see cassette.cpp).

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

    // The deck's transport keys drive these. Playback feeds bytes only with the
    // motor engaged (PLAY); recording accepts bytes only with PLAY + REC. A deck
    // at rest has no key down — motor off, not recording, not winding.
    void setMotor(bool on) {
        motor_ = on;
        if (on) wind_ = 0;                       // PLAY releases FF / REW
        else if (mode_ == kPlaying) mode_ = kIdle;
    }
    void setRecordArm(bool on) {
        rec_armed_ = on;
        if (!on && mode_ == kRecording) mode_ = kIdle;
    }
    // Wind the tape: +1 = FAST-FORWARD, -1 = REW, 0 = release. At unlimited
    // speed the seek is instant (to the far end / the start).
    void setWind(int dir);
    bool motor() const { return motor_; }
    int  wind()  const { return wind_; }

    // Advance the clock the transport is measured against, and move the head for
    // a wind or a free-running PLAY.
    void tick(uint64_t cpuCycles);
    // Bytes per second; 0 = unlimited. Measured against a 2 MHz CPU. The
    // accrued byte-credit is speed-relative, so drop it on a change.
    void setSpeed(int bytesPerSec) {
        cycles_per_byte_ = bytesPerSec > 0 ? (2000000ull / bytesPerSec) : 0;
        credit_ = 0;
    }

    // ---- host / front-end side --------------------------------------
    void mount(const uint8_t *data, std::size_t len);   // insert a tape, rewound
    void eject();
    void rewind() { pos_ = 0; head_frac_ = 0; credit_ = 0; }
    bool loaded() const { return mounted_; }     // a blank tape counts as loaded

    // The tape's current contents (a recording, or an edited playback), to save.
    const std::vector<uint8_t> &data() const { return tape_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // For the deck UI.
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
    // The physical end of the tape: room past the recording for more programs.
    std::size_t capacity() const {
        return std::max<std::size_t>(tape_.size() + kTapeSlack, kMinTape);
    }
    uint64_t    ioTicks() const { return io_ticks_; }

private:
    static constexpr std::size_t kMinTape   = 16384;  // a blank tape's usable length
    static constexpr std::size_t kTapeSlack  = 4096;  // blank room past a recording
    static constexpr int         kWindMult  = 30;     // FF / REW travel vs. play rate

    // bytes/sec of head travel at the current speed (0 = unlimited / "Max")
    double byteRate() const {
        return cycles_per_byte_ ? 2000000.0 / cycles_per_byte_ : 0.0;
    }
    // a byte-time is available to read / write (at "Max" the transfer is never gated)
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
