#pragma once
#include <cstdint>
#include <cstring>
#include "Hal.h"
// The text log ring (docs/REWRITE-PLAN.md §3.7): everything the console prints is also kept
// here, addressed by an absolute byte offset that only grows, so a web page can ask for
// "everything since offset N" and be told honestly when the oldest bytes it wanted are gone.
// Writers and readers may be on different cores; the Lock covers each call.
namespace uppity {

template <uint32_t N>
class TextRing {
  public:
    explicit TextRing(Lock& lock) : lock_(lock) {}
    void write(const char* s, size_t n) {
        lock_.lock();
        for (size_t i = 0; i < n; ++i) buf_[(total_ + i) % N] = static_cast<uint8_t>(s[i]);
        total_ += static_cast<uint32_t>(n);
        lock_.unlock();
    }
    void line(const char* s) { write(s, std::strlen(s)); write("\n", 1); }
    uint32_t total() const { return total_; }              // bytes ever written
    uint32_t oldest() const { return total_ > N ? total_ - N : 0; }
    // Copy up to `max` bytes starting at absolute offset `from`. If that offset has already
    // been overwritten the copy starts at the oldest byte held and *actualFrom says so (the
    // gap is what was lost). Returns the byte count; 0 when `from` is at (or past) the end.
    size_t read(uint32_t from, char* dst, size_t max, uint32_t* actualFrom) {
        lock_.lock();
        uint32_t old = oldest();
        if (static_cast<int32_t>(from - old) < 0) from = old;
        if (static_cast<int32_t>(from - total_) > 0) from = total_;
        size_t n = total_ - from; if (n > max) n = max;
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<char>(buf_[(from + i) % N]);
        if (actualFrom) *actualFrom = from;
        lock_.unlock();
        return n;
    }
    static constexpr uint32_t capacity() { return N; }
  private:
    Lock& lock_;
    uint8_t buf_[N] = {};
    uint32_t total_ = 0;
};

using LogRing = TextRing<16384>;

} // namespace uppity
