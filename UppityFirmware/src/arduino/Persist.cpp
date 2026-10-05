#ifdef ARDUINO
#include <Arduino.h>
#include <nvs_flash.h>
#include "Persist.h"

namespace uppity {

// ---------------------------------------------------------------- sequences

bool NvsSequenceStore::begin() { return p_.begin("uppityseq", false); }
bool NvsSequenceStore::read(int n, char* out, size_t cap) {
    if (n < 0 || n >= kCount) return false;
    char k[8]; key(k, 's', n);
    if (!p_.isKey(k)) return false;
    size_t got = p_.getString(k, out, cap);
    if (got == 0) { out[0] = '\0'; return false; }
    out[cap - 1] = '\0';
    return true;
}
bool NvsSequenceStore::write(int n, const char* c) { if (n < 0 || n >= kCount) return false; char k[8]; key(k, 's', n); return p_.putString(k, c) > 0; }
bool NvsSequenceStore::erase(int n) { if (n < 0 || n >= kCount) return false; char k[8]; key(k, 's', n); bool a = p_.isKey(k) ? p_.remove(k) : true; key(k, 'n', n); if (p_.isKey(k)) p_.remove(k); return a; }
bool NvsSequenceStore::readName(int n, char* out, size_t cap) { if (n < 0 || n >= kCount) return false; char k[8]; key(k, 'n', n); if (!p_.isKey(k)) return false; size_t got = p_.getString(k, out, cap); out[cap - 1] = '\0'; return got > 0; }
bool NvsSequenceStore::writeName(int n, const char* name) { if (n < 0 || n >= kCount) return false; char k[8]; key(k, 'n', n); if (name[0] == '\0') { if (p_.isKey(k)) p_.remove(k); return true; } return p_.putString(k, name) > 0; }
void NvsSequenceStore::clearAll() { p_.clear(); }

// ---------------------------------------------------------------- blobs

bool Persist::writeBlob(const char* key, const void* data, size_t n) {
    Preferences p;
    if (!p.begin("uppity", false)) return false;
    size_t w = p.putBytes(key, data, n);
    p.end();
    return w == n;
}

bool Persist::begin(Settings& s, Calibration& c, TextSink& log) {
    s_ = &s; c_ = &c; log_ = &log;
    seq_.begin();
    Preferences p;
    bool had = false, migrated = false, calMigrated = false;
    // opened read-write so the namespace exists from the first boot (a read-only open of a
    // namespace that does not exist yet fails, which is not "unavailable")
    if (p.begin("uppity", false)) {
        uint8_t buf[512];
        size_t n = p.isKey("settings") ? p.getBytesLength("settings") : 0;
        if (n > 0 && n <= sizeof buf) {
            p.getBytes("settings", buf, n);
            uint16_t from = migrateSettings(s, buf, n);
            if (from == 0) log_->printf("SETTINGS=unreadable (%u bytes), using defaults", static_cast<unsigned>(n));
            else if (from > kSettingsSchema) {
                // an older build flashed over a newer one: use what this build knows, leave the blob alone
                had = true;
                log_->printf("SETTINGS=from a newer build (schema %u; this build knows %u): kept, not rewritten until a setting changes", from, kSettingsSchema);
            } else {
                had = true;
                if (from != kSettingsSchema || n != sizeof(Settings)) {
                    log_->printf("SETTINGS=migrated from schema %u (%u bytes) to %u", from, static_cast<unsigned>(n), kSettingsSchema);
                    migrated = true;                   // write the upgraded blob back once, so this line does not repeat every boot
                } else log_->printf("SETTINGS=loaded schema %u", from);
            }
        } else { s = Settings{}; log_->line("SETTINGS=none (first boot of v4)"); }
        n = p.isKey("cal") ? p.getBytesLength("cal") : 0;
        if (n > 0 && n <= sizeof buf) {
            p.getBytes("cal", buf, n);
            uint16_t cfrom = migrateCalibration(c, buf, n);
            if (cfrom == 0) log_->line("CALIBRATION=unreadable, ignored");
            else { log_->printf("CALIBRATION=%s travel=%u minUp=%u minDown=%u rest=%u (profile %u)%s", c.valid ? "loaded" : "loaded (invalid)", c.travel, c.minUp, c.minDown, c.bottomRest, c.motor, cfrom > kCalibrationSchema ? " from a newer build, not rewritten" : "");
                   calMigrated = cfrom < kCalibrationSchema || (cfrom == kCalibrationSchema && n != sizeof(Calibration)); }
        } else { c = Calibration{}; }
        p.end();
    } else {
        s = Settings{}; c = Calibration{};
        log_->line("SETTINGS=NVS namespace unavailable, using defaults (nothing will persist)");
    }
    lastSaved_ = s; lastSavedCal_ = c;
    // a migrated blob is re-written in the current layout at the first idle moment (a blob
    // that merely loaded is left alone: nothing to write)
    if (migrated) { settingsDirty_ = true; std::memset(&lastSaved_, 0xFF, sizeof lastSaved_); }
    if (calMigrated) { calDirty_ = true; std::memset(&lastSavedCal_, 0xFF, sizeof lastSavedCal_); }
    return had;
}

bool Persist::queueOp(uint8_t kind, int n, const char* text) {
    if (ops_ >= kPendingOps) return false;
    Op& o = ops_buf_[ops_++];
    o.kind = kind; o.n = static_cast<uint8_t>(n);
    std::snprintf(o.text, sizeof o.text, "%s", text ? text : "");
    return true;
}
bool Persist::storeSequence(int n, const char* canonical) { return queueOp(1, n, canonical); }
bool Persist::eraseSequence(int n) { return queueOp(2, n, nullptr); }
bool Persist::nameSequence(int n, const char* name) { return queueOp(3, n, name); }
bool Persist::clearSequences() { ops_ = 0; return queueOp(4, 0, nullptr); }

void Persist::applyOp(const Op& o) {
    switch (o.kind) {
        case 1: log_->printf("SEQ %d %s", o.n, seq_.write(o.n, o.text) ? "stored" : "STORE FAILED"); break;
        case 2: log_->printf("SEQ %d %s", o.n, seq_.erase(o.n) ? "deleted" : "DELETE FAILED"); break;
        case 3: log_->printf("SEQ %d name %s", o.n, seq_.writeName(o.n, o.text) ? "stored" : "STORE FAILED"); break;
        case 4: seq_.clearAll(); log_->line("SEQ all cleared"); break;
        default: break;
    }
}

void Persist::update(const Snapshot& snap, MotionSupervisor& sup) {
    if (!s_) return;
    if (settingsDirty_ && std::memcmp(s_, &lastSaved_, sizeof(Settings)) == 0) settingsDirty_ = false;    // unchanged: nothing to write
    if (calDirty_ && std::memcmp(c_, &lastSavedCal_, sizeof(Calibration)) == 0) calDirty_ = false;
    if (!pending()) { deferredNoted_ = false; failing_ = false; return; }
    // a write that failed (NVS full, namespace unavailable) is tried again every 10 s, not on every
    // loop() pass with a log line each time (code review 2026-10)
    if (failing_ && static_cast<int32_t>(snap.tMs - retryAt_) < 0) return;
    bool disarmed = !snap.enabled && snap.activeJob == JobKind::None;
    if (!disarmed) {
        if (!deferredNoted_) { deferredNoted_ = true; log_->line("PERSIST=deferred (motors armed); it will save when the board is idle"); }
        return;
    }
    sup.setHoldIntake(MotionSupervisor::kHoldPersist, true);
    if (settingsDirty_) {
        s_->size = sizeof(Settings); s_->schema = kSettingsSchema;
        bool ok = writeBlob("settings", s_, sizeof(Settings));
        if (ok) { lastSaved_ = *s_; ++saves_; }
        if (ok || !failing_) log_->printf("PERSIST=%s settings", ok ? "saved" : "WRITE FAILED (trying again every 10 s)");
        settingsDirty_ = !ok;
        if (!ok) { failing_ = true; retryAt_ = snap.tMs + 10000; }
    }
    if (calDirty_) {
        c_->size = sizeof(Calibration); c_->schema = kCalibrationSchema;
        bool ok = writeBlob("cal", c_, sizeof(Calibration));
        if (ok) { lastSavedCal_ = *c_; ++saves_; }
        if (ok || !failing_) log_->printf("PERSIST=%s calibration", ok ? "saved" : "WRITE FAILED (trying again every 10 s)");
        calDirty_ = !ok;
        if (!ok) { failing_ = true; retryAt_ = snap.tMs + 10000; }
    }
    for (int i = 0; i < ops_; ++i) applyOp(ops_buf_[i]);
    ops_ = 0;
    sup.setHoldIntake(MotionSupervisor::kHoldPersist, false);
    deferredNoted_ = false;
}

// v3 kept its tuning in NVS namespace "uppityspinner"; the v4 flash layout leaves that
// partition where it was, so the builder's numbers are still readable. Read-only.
bool Persist::readV3(V3Prefs& out) {
    out = V3Prefs{};
    Preferences p;
    if (!p.begin("uppityspinner", true)) return false;
    auto get = [&](const char* k) -> int32_t {
        if (!p.isKey(k)) return -1;
        PreferenceType t = p.getType(k);
        switch (t) {
            case PT_I8:  return p.getChar(k, -1);
            case PT_U8:  return p.getUChar(k, 0);
            case PT_I16: return p.getShort(k, -1);
            case PT_U16: return p.getUShort(k, 0);
            case PT_I32: return p.getInt(k, -1);
            case PT_U32: return static_cast<int32_t>(p.getUInt(k, 0));
            default:     return -1;
        }
    };
    out.lftminpwr = get("lftminpwr"); out.lftseekbotpwr = get("lftseekbotpwr"); out.rotminpwr = get("rotminpwr");
    out.lftdist = get("lftdist"); out.minheight = get("minheight"); out.rotenccount = get("rotenccount");
    out.driftpct = get("driftpct"); out.aggression = get("aggression"); out.motortype = get("motortype");
    out.expertmode = get("expertmode"); out.wizstate = get("wizstate");
    out.present = out.lftminpwr >= 0 || out.lftdist >= 0 || out.minheight >= 0 || out.motortype >= 0 || out.rotenccount >= 0;
    p.end();
    return true;
}

void Persist::factoryReset() {
    Preferences p;
    if (p.begin("uppity", false)) { p.clear(); p.end(); }
    seq_.clearAll();
    settingsDirty_ = calDirty_ = false; ops_ = 0;
    log_->line("FACTORY=cleared settings, calibration and sequences (v3's stored values untouched)");
}

} // namespace uppity
#endif
