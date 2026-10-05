#pragma once
#include <cstdarg>
#include <cstdio>
// Where the command layer writes its KEY=VALUE answers and SEQ/RANDOM/PERSIST notes. The
// console implements it over the USB serial; tests collect the lines in a vector.
namespace uppity {
struct TextSink {
    virtual ~TextSink() = default;
    virtual void line(const char* text) = 0;
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[300];          // a #PS line with a full 254-character stored sequence must print whole (#PCONFIG replays it)
        va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        line(buf);
    }
};
struct NullSink final : TextSink { void line(const char*) override {} };

// Random numbers for the command layer: esp_random() on the board, a seeded xorshift in tests.
struct RandomSource {
    virtual ~RandomSource() = default;
    virtual uint32_t next() = 0;
    int32_t range(int32_t lo, int32_t hi) { if (hi <= lo) return lo; return lo + static_cast<int32_t>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};
struct XorShiftRandom final : RandomSource {
    uint32_t s;
    explicit XorShiftRandom(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    uint32_t next() override { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};
} // namespace uppity
