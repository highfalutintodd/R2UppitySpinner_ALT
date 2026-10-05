#pragma once
#include <cstdint>
#include <cstring>
#include "Units.h"
// Per-source serial ingestion (docs/REWRITE-PLAN.md §3.4):
//  * EstopScanner runs in the RX callback, ahead of any buffer, and trips the latch the moment
//    ":PX", "?STOP", "&ESTOP" or "&SABE,ESTOP" arrives — even mid-chain, even if loop() is busy.
//  * SerialFifo carries bytes from the RX callback to loop().
//  * LineAssembler builds one line per source and decides acceptance by the FIRST byte: ':' and
//    '#' are ours; '&' lines are ecosystem verbs; everything else (heartbeats, WDP chatter,
//    other boards) is dropped whole, terminator included, and never answered.
namespace uppity {

class EstopScanner {
  public:
    // Returns true exactly when a complete trigger has just been seen.
    bool feed(char c) {
        bool trip = false;
        for (int i = 0; i < kPatterns; ++i) {
            const char* p = kPattern[i];
            int& n = idx_[i];
            if (p[n] == '\0') {                          // pattern complete: need the terminator
                if (c == '\r' || c == '\n' || (kCommaOk[i] && c == ',')) trip = true;
                n = (c == p[0]) ? 1 : 0;
                continue;
            }
            if (c == p[n]) ++n;
            else n = (c == p[0]) ? 1 : 0;
        }
        return trip;
    }
    void reset() { for (int i = 0; i < kPatterns; ++i) idx_[i] = 0; }
  private:
    static constexpr int kPatterns = 4;
    static constexpr const char* kPattern[kPatterns] = { ":PX", "?STOP", "&ESTOP", "&SABE,ESTOP" };
    static constexpr bool kCommaOk[kPatterns] = { false, false, false, true };
    int idx_[kPatterns] = {0, 0, 0, 0};
};

// A byte that never occurs on our lines (ASCII CAN): it marks where bytes were lost, so the line
// they belonged to is dropped instead of splicing its head onto a later line's tail (code review
// 2026-10, finding 11: "#PSAFESPIN878" + lost bytes + "0" from "#PDRIFT=0" read as "#PSAFESPIN870").
constexpr char kLostBytes = 0x18;

// Single-producer / single-consumer byte ring (RX callback -> loop()). When a byte cannot be
// stored, the next byte that can is preceded by kLostBytes. markLost() does the same for bytes the
// UART itself dropped before they reached this ring.
template <int N>
class SerialFifo {
  public:
    bool push(uint8_t b) {
        uint32_t h = head_, t = tail_;
        if (lost_) { if (h - t + 1 >= static_cast<uint32_t>(N)) { dropped_ = dropped_ + 1; return false; } buf_[h % N] = static_cast<uint8_t>(kLostBytes); ++h; lost_ = false; }
        if (h - t >= static_cast<uint32_t>(N)) { dropped_ = dropped_ + 1; lost_ = true; return false; }
        buf_[h % N] = b; __sync_synchronize(); head_ = h + 1; return true;
    }
    void markLost() { lost_ = true; }
    bool pop(uint8_t& b) { uint32_t t = tail_, h = head_; if (t == h) return false; b = buf_[t % N]; __sync_synchronize(); tail_ = t + 1; return true; }
    int available() const { return static_cast<int>(head_ - tail_); }
    uint32_t dropped() const { return dropped_; }
  private:
    uint8_t buf_[N];
    volatile uint32_t head_ = 0, tail_ = 0, dropped_ = 0;
    bool lost_ = false;                 // producer side only
};

class LineAssembler {
  public:
    // The longest line #PCONFIG prints is "#PS100:" plus a 254-character stored sequence; a line
    // that does not fit is dropped whole, never cut short and run (code review 2026-10).
    static constexpr int kMax = 272;
    enum class Kind : uint8_t { None, Command, Ecosystem, Dropped };

    // Feed a byte; returns Command/Ecosystem when a line for us just completed (take it with
    // line()), Dropped when a foreign line was discarded, None otherwise.
    Kind feed(char c) {
        if (c == kLostBytes) { dropping_ = true; return Kind::None; }  // bytes were lost: this line is not what was sent
        if (c == '\r' || c == '\n') {
            if (len_ == 0 && !dropping_) return Kind::None;          // bare terminator (CRLF second half)
            Kind k = Kind::None;
            if (dropping_ || overflow_) { k = Kind::Dropped; ++dropped_; }
            else { buf_[len_] = '\0'; ready_ = true; k = buf_[0] == '&' ? Kind::Ecosystem : Kind::Command; }
            dropping_ = false; len_ = 0; overflow_ = false;
            if (k == Kind::Dropped) ready_ = false;
            return k;
        }
        if (dropping_) return Kind::None;
        if (len_ == 0) {
            if (c == ':' || c == '#' || c == '&') { buf_[len_++] = c; return Kind::None; }
            dropping_ = true; return Kind::None;                      // foreign line: swallow until its end
        }
        if (len_ < kMax - 1) buf_[len_++] = c; else overflow_ = true;
        return Kind::None;
    }
    const char* line() const { return buf_; }
    bool overflowed() const { return overflow_; }
    uint32_t droppedLines() const { return dropped_; }
    void reset() { len_ = 0; dropping_ = false; ready_ = false; overflow_ = false; }
  private:
    char buf_[kMax] = {};
    int len_ = 0;
    bool dropping_ = false, ready_ = false, overflow_ = false;
    uint32_t dropped_ = 0;
};

} // namespace uppity
