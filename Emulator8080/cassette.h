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
// the "tape" is just a byte buffer with a play/record head position. Recording
// and playback each rewind to the start, and the front end can rewind by hand —
// exactly the ritual you did with a real deck.
//
// Status-bit polarity is confirmed against our 8K BASIC ROM (see cassette.cpp).

#ifndef EMULATOR8080_CASSETTE_H
#define EMULATOR8080_CASSETTE_H

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

    // ---- host / front-end side --------------------------------------
    void mount(const uint8_t *data, std::size_t len);   // insert a tape, rewound
    void eject();
    void rewind() { pos_ = 0; }
    bool loaded() const { return !tape_.empty(); }

    // The tape's current contents (a recording, or an edited playback), to save.
    const std::vector<uint8_t> &data() const { return tape_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // For the deck UI.
    enum Mode { kIdle, kPlaying, kRecording };
    Mode        mode()  const { return mode_; }
    std::size_t pos()   const { return pos_; }
    std::size_t len()   const { return tape_.size(); }
    uint64_t    ioTicks() const { return io_ticks_; }

private:
    std::vector<uint8_t> tape_;
    std::size_t pos_       = 0;
    Mode        mode_      = kIdle;
    bool        dirty_     = false;
    uint64_t    io_ticks_  = 0;
    int         idle_polls_ = 0;   // consecutive read-status polls at end of tape
};

} // namespace altair

#endif // EMULATOR8080_CASSETTE_H
