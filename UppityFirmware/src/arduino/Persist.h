#pragma once
#ifdef ARDUINO
#include <Preferences.h>
#include "../core/Settings.h"
#include "../core/SequenceStore.h"
#include "../core/StatusSnapshot.h"
#include "../core/MotionSupervisor.h"
#include "../core/TextSink.h"
// The only writer of v4's flash state (docs/REWRITE-PLAN.md §3.5). Three NVS namespaces:
// "uppity" (settings blob + calibration blob), "uppityseq" (sequences and names), and v3's
// "uppityspinner", read once and never written. A flash erase stalls both cores while the
// LEDC keeps emitting, so nothing is written while the drivers are enabled or a job runs:
// writes are queued, logged as deferred, and land the moment the board is disarmed, with the
// supervisor's intake held for the few milliseconds of the commit.
namespace uppity {

class NvsSequenceStore final : public SequenceStore {
  public:
    bool begin();
    bool read(int n, char* out, size_t cap) override;
    bool write(int n, const char* canonical) override;
    bool erase(int n) override;
    bool readName(int n, char* out, size_t cap) override;
    bool writeName(int n, const char* name) override;
    void clearAll() override;
  private:
    static void key(char* buf, char prefix, int n) { std::snprintf(buf, 8, "%c%d", prefix, n); }
    Preferences p_;
};

class Persist {
  public:
    static constexpr int kPendingOps = 6;
    bool begin(Settings& s, Calibration& c, TextSink& log);     // loads; returns true if a v4 settings blob existed
    void requestSaveSettings() { settingsDirty_ = true; }
    void requestSaveCalibration() { calDirty_ = true; }
    bool storeSequence(int n, const char* canonical);           // false = pending queue full
    bool eraseSequence(int n);
    bool nameSequence(int n, const char* name);
    bool clearSequences();
    void update(const Snapshot& snap, MotionSupervisor& sup);  // every loop(): flush when disarmed
    bool pending() const { return settingsDirty_ || calDirty_ || ops_ > 0; }
    bool readV3(V3Prefs& out);
    void factoryReset();                                        // clears the v4 namespaces (v3's stays)
    SequenceStore& sequences() { return seq_; }
    uint32_t saves() const { return saves_; }
  private:
    struct Op { uint8_t kind; uint8_t n; char text[SequenceStore::kMaxLen + 1]; };   // kind: 1 write, 2 erase, 3 name, 4 clear
    bool queueOp(uint8_t kind, int n, const char* text);
    void applyOp(const Op& o);
    bool writeBlob(const char* key, const void* data, size_t n);
    Settings* s_ = nullptr; Calibration* c_ = nullptr; TextSink* log_ = nullptr;
    Settings lastSaved_; Calibration lastSavedCal_;
    NvsSequenceStore seq_;
    Op ops_buf_[kPendingOps]; int ops_ = 0;
    bool settingsDirty_ = false, calDirty_ = false, deferredNoted_ = false;
    uint32_t saves_ = 0;
};

} // namespace uppity
#endif
