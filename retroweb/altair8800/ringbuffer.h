// Fixed-capacity single-producer / single-consumer byte ring buffer.
//
// One side (the CPU, or the host) pushes; the other pops. It never grows and
// never throws: a push into a full buffer reports failure so the caller can
// raise an overrun; a pop from an empty buffer reports failure.

#ifndef EMULATOR8080_RINGBUFFER_H
#define EMULATOR8080_RINGBUFFER_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace altair {

template <std::size_t N>
class RingBuffer {
    static_assert(N >= 2, "ring buffer needs at least 2 slots");

public:
    bool        empty() const { return head_ == tail_; }
    bool        full()  const { return next(head_) == tail_; }
    std::size_t size()  const { return (head_ + N - tail_) % N; }
    std::size_t capacity() const { return N - 1; }   // one slot kept free

    // Push one byte. Returns false (and drops the byte) if full.
    bool push(uint8_t v) {
        if (full()) return false;
        buf_[head_] = v;
        head_ = next(head_);
        return true;
    }

    // Pop one byte into `out`. Returns false if empty.
    bool pop(uint8_t &out) {
        if (empty()) return false;
        out = buf_[tail_];
        tail_ = next(tail_);
        return true;
    }

    // Look at the next byte without removing it.
    bool peek(uint8_t &out) const {
        if (empty()) return false;
        out = buf_[tail_];
        return true;
    }

    void clear() { head_ = tail_ = 0; }

private:
    static std::size_t next(std::size_t i) { return (i + 1) % N; }

    std::array<uint8_t, N> buf_{};
    std::size_t head_ = 0;   // write index
    std::size_t tail_ = 0;   // read index
};

} // namespace altair

#endif // EMULATOR8080_RINGBUFFER_H
