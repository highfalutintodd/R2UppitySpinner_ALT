#pragma once
#include <cstdint>
#include <cstring>
// Stored sequences 0..100 (":PS<n>" replays them, "#PS<n>:..." stores them), in canonical
// ":PP100:PL5:PR30" form, with optional names. NVS strings on the board, RAM in tests.
namespace uppity {

class SequenceStore {
  public:
    static constexpr int kCount = 101;         // v3 clamped n to 0..100
    static constexpr int kMaxLen = 255;
    static constexpr int kNameLen = 31;
    virtual ~SequenceStore() = default;
    virtual bool read(int n, char* out, size_t cap) = 0;      // false = none stored
    virtual bool write(int n, const char* canonical) = 0;
    virtual bool erase(int n) = 0;
    virtual bool readName(int n, char* out, size_t cap) = 0;
    virtual bool writeName(int n, const char* name) = 0;
    virtual void clearAll() = 0;
};

class MemSequenceStore final : public SequenceStore {
  public:
    bool read(int n, char* out, size_t cap) override { if (!ok(n) || !has_[n]) return false; std::snprintf(out, cap, "%s", text_[n]); return true; }
    bool write(int n, const char* c) override { if (!ok(n)) return false; std::snprintf(text_[n], sizeof text_[n], "%s", c); has_[n] = true; return true; }
    bool erase(int n) override { if (!ok(n)) return false; has_[n] = false; name_[n][0] = '\0'; return true; }
    bool readName(int n, char* out, size_t cap) override { if (!ok(n) || name_[n][0] == '\0') return false; std::snprintf(out, cap, "%s", name_[n]); return true; }
    bool writeName(int n, const char* name) override { if (!ok(n)) return false; std::snprintf(name_[n], sizeof name_[n], "%s", name); return true; }
    void clearAll() override { for (int i = 0; i < kCount; ++i) { has_[i] = false; name_[i][0] = '\0'; } }
  private:
    static bool ok(int n) { return n >= 0 && n < kCount; }
    char text_[kCount][kMaxLen + 1] = {};
    char name_[kCount][kNameLen + 1] = {};
    bool has_[kCount] = {};
};

} // namespace uppity
