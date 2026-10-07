// Fixed-capacity single-producer / single-consumer byte ring buffer. Never grows
// or throws; push into a full buffer and pop from an empty one return false.

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

    bool push(uint8_t v) {
        if (full()) return false;
        buf_[head_] = v;
        head_ = next(head_);
        return true;
    }

    bool pop(uint8_t &out) {
        if (empty()) return false;
        out = buf_[tail_];
        tail_ = next(tail_);
        return true;
    }

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
