#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "Hal.h"
// Command lines from another task (the web server, later the mesh) to loop(), where the
// dispatcher runs. Fixed slots, copied in and out under the Lock; a full queue says so.
namespace uppity {

template <int Slots, int Len>
class LineQueue {
  public:
    explicit LineQueue(Lock& lock) : lock_(lock) {}
    // A line that does not fit is refused whole (counted as dropped), never cut short: a cut line
    // could still run ("…:PP100" read as "…:PP10").
    bool push(const char* line, uint8_t tag = 0) {
        if (!line || std::strlen(line) >= static_cast<size_t>(Len)) { lock_.lock(); ++dropped_; lock_.unlock(); return false; }
        lock_.lock();
        bool ok = count_ < Slots;
        if (ok) { int i = (head_ + count_) % Slots; std::snprintf(buf_[i], Len, "%s", line); tag_[i] = tag; ++count_; }
        else ++dropped_;
        lock_.unlock();
        return ok;
    }
    bool pop(char* out, size_t cap, uint8_t* tag = nullptr) {
        lock_.lock();
        bool ok = count_ > 0;
        if (ok) { std::snprintf(out, cap, "%s", buf_[head_]); if (tag) *tag = tag_[head_]; head_ = (head_ + 1) % Slots; --count_; }
        lock_.unlock();
        return ok;
    }
    int size() { lock_.lock(); int n = count_; lock_.unlock(); return n; }
    uint32_t dropped() const { return dropped_; }
  private:
    Lock& lock_;
    char buf_[Slots][Len] = {};
    uint8_t tag_[Slots] = {};
    int head_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};

using WebLineQueue = LineQueue<8, 256>;

} // namespace uppity
