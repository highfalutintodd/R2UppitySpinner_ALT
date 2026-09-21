#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
// The one JSON writer (docs/REWRITE-PLAN.md §3.6): every string goes through the escaper, so a
// stray quote or control character in a log line or an SSID can never produce invalid JSON
// (v3 escaped only '"'). Fixed buffer, never allocates, reports overflow instead of truncating
// silently.
namespace uppity {

class JsonWriter {
  public:
    JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) { if (cap_) buf_[0] = '\0'; }

    void beginObject() { sep(); put('{'); push(); }
    void endObject()   { pop(); put('}'); }
    void beginArray()  { sep(); put('['); push(); }
    void endArray()    { pop(); put(']'); }
    void key(const char* k) { sep(); quoted(k); put(':'); pendingValue_ = true; }
    void str(const char* v) { sep(); quoted(v ? v : ""); }
    void num(long v) { sep(); char t[24]; std::snprintf(t, sizeof t, "%ld", v); puts(t); }
    void unum(unsigned long v) { sep(); char t[24]; std::snprintf(t, sizeof t, "%lu", v); puts(t); }
    void numf(double v, int decimals) { sep(); char t[32]; std::snprintf(t, sizeof t, "%.*f", decimals, v); puts(t); }
    void boolean(bool b) { sep(); puts(b ? "true" : "false"); }
    void null() { sep(); puts("null"); }
    // key/value shorthands
    void kv(const char* k, const char* v) { key(k); str(v); }
    void kv(const char* k, long v) { key(k); num(v); }
    void kv(const char* k, int v) { key(k); num(v); }
    void kv(const char* k, unsigned long v) { key(k); unum(v); }
    void kv(const char* k, bool v) { key(k); boolean(v); }
    void kvf(const char* k, double v, int decimals) { key(k); numf(v, decimals); }

    bool overflowed() const { return overflow_; }
    size_t length() const { return len_; }
    const char* c_str() const { return buf_; }

  private:
    static constexpr int kDepth = 8;
    void push() { if (depth_ < kDepth) first_[depth_] = true; ++depth_; pendingValue_ = false; }
    void pop() { if (depth_ > 0) --depth_; pendingValue_ = false; }
    // a comma before every element except the first of its container; never after a key
    void sep() {
        if (pendingValue_) { pendingValue_ = false; return; }
        if (depth_ > 0 && depth_ <= kDepth) { if (!first_[depth_ - 1]) put(','); first_[depth_ - 1] = false; }
    }
    void put(char c) { if (len_ + 1 < cap_) { buf_[len_++] = c; buf_[len_] = '\0'; } else overflow_ = true; }
    void puts(const char* s) { while (*s) put(*s++); }
    void quoted(const char* s) {
        put('"');
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            unsigned char c = *p;
            switch (c) {
                case '"': puts("\\\""); break;
                case '\\': puts("\\\\"); break;
                case '\n': puts("\\n"); break;
                case '\r': puts("\\r"); break;
                case '\t': puts("\\t"); break;
                default:
                    if (c < 0x20 || c == 0x7f) { char t[8]; std::snprintf(t, sizeof t, "\\u%04x", c); puts(t); }
                    else put(static_cast<char>(c));
            }
        }
        put('"');
    }
    char* buf_; size_t cap_; size_t len_ = 0;
    bool first_[kDepth] = {}; int depth_ = 0;
    bool pendingValue_ = false, overflow_ = false;
};

} // namespace uppity
