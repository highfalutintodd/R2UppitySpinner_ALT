#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "Units.h"
#include "Profiles.h"
#include "MotionSupervisor.h"
// Persistent settings (docs/REWRITE-PLAN.md §3.5, Appendix B.2). Pure: the struct, its
// defaults, the clamps applied on load, the prefix-copy migration, the KEY=VALUE registry
// behind "#P<KEY>=<VALUE>" / "#PCONFIG", the calibration blob, and the one-shot v3 import.
// The Arduino side (Persist) only moves bytes between here and NVS.
//
// Layout rules, because the blob is copied straight out of memory:
//   * fixed-width types only, ordered 4-byte, 2-byte, 1-byte so there is no implicit padding
//     (static_assert below), then a zeroed reserve at the end;
//   * new fields take bytes from the reserve and bump kSettingsSchema; migrate() gives them
//     their default when an older blob is loaded; nothing is ever moved or retyped.
namespace uppity {

constexpr uint16_t kSettingsSchema = 2;

struct Settings {
    // ---- 4-byte ----
    uint32_t cmdBaud = 9600;         // command UART (Marcduino bus)
    float    ticksPerRev = 0.f;      // rotary; 0 = unknown (first rotary use runs the two-turn measurement)
    // ---- 2-byte ----
    uint16_t schema = kSettingsSchema;
    uint16_t size = 0;               // sizeof(Settings) when saved; migration reports it
    uint16_t travelTicks = 0;        // builder's travel (#PTRAVEL); 0 = calibrated value, else profile default
    uint16_t safeSpinTicks = 0;      // lifter height above which the head clears the dome; 0 = only the top counts
    uint16_t contactArcTicks = 0;    // rotary switch arc while moving, from characterisation
    uint16_t dedupMs = 750;          // identical lines from different transports inside this window run once
    int16_t  homeOffsetTicks = 0;    // visual home relative to the switch arc centre
    // ---- 1-byte ----
    uint8_t  marcId = 0;             // ":P<id>..." lines with another id are dropped silently
    uint8_t  motor = 1;              // LifterMotor; 19:1 is the protective default (bench 2026-09-19)
    uint8_t  minPowerUpPct = 0;      // 0 = profile default
    uint8_t  minPowerDownPct = 0;
    uint8_t  approachPowerPct = 0;   // seek-bottom / soft-approach floor; 0 = profile default
    uint8_t  inverted = 0;           // mechanism runs backwards (v3 fLifterInverted)
    uint8_t  lifterLimitsNC = 0;     // 0 = normally open to ground (made reads LOW), 1 = normally closed
    uint8_t  rotaryHomeNC = 0;
    uint8_t  expertMode = 0;         // lifts the 19:1 throttle caps
    uint8_t  driftPct = 5;           // hold re-seeks when off by more than this; 0 = off
    uint8_t  tprConfident = 0;
    uint8_t  chatterPerPass = 0;
    uint8_t  rotaryMinPct = 40;      // floor for normal rotary moves
    uint8_t  rotaryDisabled = 0;
    int8_t   driveSign = -1;         // +1: positive throttle increases the rotary count; v3 wiring is -1
    uint8_t  homePark = 1;           // 0 arc centre by count; 1 creep in against the sweep (v3's spot); 2 with the sweep
    uint8_t  wiggleDeg = 2;          // first swing of the settle wiggle before parking; 0 = none
    uint8_t  aggressiveness = 1;     // random mode default level: 0 gentle, 1 medium, 2 aggressive
    uint8_t  commissioned = 0;       // 0 locked; 1 bench unlock (#PUNLOCK); 2 commissioning flow (phase 4)
    uint8_t  debug = 0;              // verbose motion tracing
    uint8_t  v3Imported = 0;         // the one-shot v3 NVS import has run
    uint8_t  cmdTx = 0;              // echo responses on the command UART TX (GPIO0, only if wired)
    uint8_t  randomLight = 0;        // schema 2: light kit while random mode runs; 0 leave alone, 1-7 that mode, 8 shuffle per action; off when it ends
    uint8_t  reserved[35] = {};      // zero; new fields are carved from here
};
static_assert(sizeof(Settings) == 80, "Settings layout changed: append inside reserved[] only and bump kSettingsSchema");

// Clamp every field into its legal range. Runs after every load and every set.
inline void sanitize(Settings& s) {
    if (s.cmdBaud < 1200 || s.cmdBaud > 921600) s.cmdBaud = 9600;
    if (!(s.ticksPerRev >= 100.f && s.ticksPerRev <= 5000.f)) s.ticksPerRev = 0.f;
    if (s.ticksPerRev == 0.f) s.tprConfident = 0;
    if (s.travelTicks != 0 && (s.travelTicks < 100 || s.travelTicks > 5000)) s.travelTicks = 0;
    if (s.safeSpinTicks > 5000) s.safeSpinTicks = 0;
    if (s.dedupMs > 5000) s.dedupMs = 750;
    if (s.homeOffsetTicks < -600 || s.homeOffsetTicks > 600) s.homeOffsetTicks = 0;
    if (s.motor >= static_cast<uint8_t>(LifterMotor::Count)) s.motor = 1;
    if (s.minPowerUpPct > 100) s.minPowerUpPct = 0;
    if (s.minPowerDownPct > 100) s.minPowerDownPct = 0;
    if (s.approachPowerPct > 100) s.approachPowerPct = 0;
    s.inverted = s.inverted ? 1 : 0; s.lifterLimitsNC = s.lifterLimitsNC ? 1 : 0; s.rotaryHomeNC = s.rotaryHomeNC ? 1 : 0;
    s.expertMode = s.expertMode ? 1 : 0; s.tprConfident = s.tprConfident ? 1 : 0; s.rotaryDisabled = s.rotaryDisabled ? 1 : 0;
    s.debug = s.debug ? 1 : 0; s.v3Imported = s.v3Imported ? 1 : 0; s.cmdTx = s.cmdTx ? 1 : 0;
    if (s.driftPct > 20) s.driftPct = 5;
    if (s.rotaryMinPct < 10 || s.rotaryMinPct > 100) s.rotaryMinPct = 40;
    if (s.driveSign != 1 && s.driveSign != -1) s.driveSign = -1;
    if (s.homePark > 2) s.homePark = 1;
    if (s.wiggleDeg > 20) s.wiggleDeg = 2;
    if (s.aggressiveness > 2) s.aggressiveness = 1;
    if (s.commissioned > 2) s.commissioned = 0;
    if (s.randomLight > 8) s.randomLight = 0;
}

// Load `stored` bytes (a blob written by this or an earlier schema) over defaults. Returns
// the schema the blob carried, 0 if the blob was unusable (too short or from the future).
inline uint16_t migrateSettings(Settings& out, const uint8_t* blob, size_t n) {
    out = Settings{};
    if (n < 8) return 0;                                     // not even the header
    Settings tmp{};
    size_t take = n < sizeof(Settings) ? n : sizeof(Settings);
    std::memcpy(&tmp, blob, take);
    if (tmp.schema == 0 || tmp.schema > kSettingsSchema) return 0;
    // schema-specific defaults for fields that did not exist when the blob was written go here
    // (a reserve byte reads as 0, so a field whose default is 0 needs nothing):
    if (tmp.schema < 2) tmp.randomLight = 0;
    out = tmp;
    out.schema = kSettingsSchema;
    sanitize(out);
    return tmp.schema;
}

// ---------------------------------------------------------------- calibration blob

constexpr uint16_t kCalibrationSchema = 1;

// The lifter calibration table, stored apart from Settings so re-running #PSC never touches
// anything else and a profile change can invalidate it alone.
struct Calibration {
    uint32_t measuredAtMs = 0;                 // uptime at the measurement (no clock on the board)
    uint16_t schema = kCalibrationSchema;
    uint16_t size = 0;
    uint16_t travel = 0;                       // ticks, rest position after the creep's brake
    uint16_t minUp = 0, minDown = 0;           // permille
    uint16_t velUp[kSlots] = {}, velDown[kSlots] = {};        // ticks per second per 5 % slot
    uint16_t overUp[kSlots] = {}, overDown[kSlots] = {};      // coast ticks after the brake
    uint8_t  slotValidUp[kSlots] = {}, slotValidDown[kSlots] = {};
    uint8_t  valid = 0;
    uint8_t  motor = 0xFF;                     // profile it was measured under; another profile ignores it
    uint8_t  reserved[30] = {};
};
static_assert(sizeof(Calibration) == 256, "Calibration layout changed");

inline uint16_t migrateCalibration(Calibration& out, const uint8_t* blob, size_t n) {
    out = Calibration{};
    if (n < 8) return 0;
    Calibration tmp{};
    size_t take = n < sizeof(Calibration) ? n : sizeof(Calibration);
    std::memcpy(&tmp, blob, take);
    if (tmp.schema == 0 || tmp.schema > kCalibrationSchema) return 0;
    out = tmp; out.schema = kCalibrationSchema;
    if (out.travel < 100 || out.travel > 5000) out.valid = 0;
    return tmp.schema;
}

inline void takeCalibration(Calibration& c, const CalibrateResult& r, uint8_t motor, Ms now) {
    c = Calibration{};
    c.measuredAtMs = now; c.travel = static_cast<uint16_t>(clampv<Ticks>(r.travel, 0, 65535));
    c.minUp = static_cast<uint16_t>(r.minUp); c.minDown = static_cast<uint16_t>(r.minDown);
    for (int i = 0; i < kSlots; ++i) { c.velUp[i] = r.velUp[i]; c.velDown[i] = r.velDown[i]; c.overUp[i] = r.overUp[i]; c.overDown[i] = r.overDown[i]; c.slotValidUp[i] = r.slotValidUp[i]; c.slotValidDown[i] = r.slotValidDown[i]; }
    c.valid = r.travelOk ? 1 : 0; c.motor = motor;
}

// ---------------------------------------------------------------- Settings -> MotionConfig

inline bool calibrationApplies(const Settings& s, const Calibration& c) { return c.valid && c.motor == s.motor; }

// Effective travel: the builder's explicit figure wins, then the calibration, then the profile.
inline Ticks effectiveTravel(const Settings& s, const Calibration& c) {
    if (s.travelTicks) return s.travelTicks;
    if (calibrationApplies(s, c)) return c.travel;
    return profileFor(static_cast<LifterMotor>(s.motor)).defaultTravelTicks;
}

inline MotionConfig toMotionConfig(const Settings& s, const Calibration& c) {
    MotionConfig m;
    m.motor = static_cast<LifterMotor>(s.motor);
    m.expertMode = s.expertMode != 0;
    bool cal = calibrationApplies(s, c);
    m.minPowerUp   = s.minPowerUpPct   ? pctToThrottle(s.minPowerUpPct)   : (cal ? static_cast<Throttle>(c.minUp)   : 0);
    m.minPowerDown = s.minPowerDownPct ? pctToThrottle(s.minPowerDownPct) : (cal ? static_cast<Throttle>(c.minDown) : 0);
    m.approachPower = s.approachPowerPct ? pctToThrottle(s.approachPowerPct) : 0;
    m.travelTicks = s.travelTicks ? s.travelTicks : (cal ? c.travel : 0);
    m.travelCalibrated = s.travelTicks != 0 || cal;
    m.lifterInverted = s.inverted != 0;
    m.safeSpinTicks = s.safeSpinTicks;
    m.rotary.ticksPerRev = s.ticksPerRev; m.rotary.confident = s.tprConfident != 0;
    m.rotary.contactArcTicks = s.contactArcTicks; m.rotary.chatterPerPass = s.chatterPerPass;
    m.rotary.minPowerPct = s.rotaryMinPct; m.rotary.driveSign = s.driveSign;
    m.rotary.homeOffsetTicks = s.homeOffsetTicks;
    m.rotary.parkOnLeadingEdge = s.homePark != 0; m.rotary.parkApproach = s.homePark == 2 ? 1 : -1;
    m.rotary.settleWiggleDeg = s.wiggleDeg;
    m.rotaryDisabled = s.rotaryDisabled != 0;
    m.commissioned = s.commissioned != 0;
    m.driftPct = s.driftPct;
    if (cal) for (int i = 0; i < kSlots; ++i) { m.overshootUp[i] = c.overUp[i]; m.overshootDown[i] = c.overDown[i]; m.velUp[i] = c.velUp[i]; m.velDown[i] = c.velDown[i]; }
    return m;
}

// The supervisor refines the rotary characterisation itself after a successful #PROTARYTEST;
// pull those numbers back into the settings so they persist.
inline bool takeCharacterisation(Settings& s, const RotaryCharacterisation& rc) {
    bool changed = s.ticksPerRev != rc.ticksPerRev || s.tprConfident != (rc.confident ? 1 : 0) || s.contactArcTicks != rc.contactArcTicks || s.chatterPerPass != rc.chatterPerPass;
    s.ticksPerRev = rc.ticksPerRev; s.tprConfident = rc.confident ? 1 : 0; s.contactArcTicks = rc.contactArcTicks; s.chatterPerPass = rc.chatterPerPass;
    return changed;
}

// ---------------------------------------------------------------- KEY=VALUE registry

// Every builder-settable field, by the name used in "#P<KEY><value>", "#P<KEY>=<value>" and
// the replayable "#PCONFIG" dump. Values are integers; ROTARYTPR is in tenths of a tick.
struct SettingKey {
    const char* name;
    int32_t (*get)(const Settings&);
    bool    (*set)(Settings&, int32_t);         // false = out of range (nothing changed)
    const char* help;
};

namespace setkey {
inline bool inRange(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
}

inline const SettingKey kSettingKeys[] = {
    { "INVERT",     [](const Settings& s) { return (int32_t)s.inverted; },        [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.inverted = (uint8_t)v; return true; }, "0/1 mechanism runs backwards" },
    { "TRAVEL",     [](const Settings& s) { return (int32_t)s.travelTicks; },     [](Settings& s, int32_t v) { if (v != 0 && !setkey::inRange(v, 100, 5000)) return false; s.travelTicks = (uint16_t)v; return true; }, "ticks of lifter travel; 0 = calibrated/profile" },
    { "SAFESPIN",   [](const Settings& s) { return (int32_t)s.safeSpinTicks; },   [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 5000)) return false; s.safeSpinTicks = (uint16_t)v; return true; }, "ticks above which the head may turn; 0 = top only" },
    { "ROTARYTPR",  [](const Settings& s) { return (int32_t)(s.ticksPerRev * 10.f + 0.5f); }, [](Settings& s, int32_t v) { if (v != 0 && !setkey::inRange(v, 1000, 50000)) return false; s.ticksPerRev = v / 10.f; s.tprConfident = v ? 1 : 0; return true; }, "rotary ticks per rev in tenths (11855 = 1185.5); 0 = measure" },
    { "HOMEOFFSET", [](const Settings& s) { return (int32_t)s.homeOffsetTicks; }, [](Settings& s, int32_t v) { if (!setkey::inRange(v, -600, 600)) return false; s.homeOffsetTicks = (int16_t)v; return true; }, "ticks from the switch centre to the visual home" },
    { "HOMEPARK",   [](const Settings& s) { return (int32_t)s.homePark; },        [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 2)) return false; s.homePark = (uint8_t)v; return true; }, "0 arc centre; 1 against the sweep (v3); 2 with the sweep" },
    { "WIGGLE",     [](const Settings& s) { return (int32_t)s.wiggleDeg; },       [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 20)) return false; s.wiggleDeg = (uint8_t)v; return true; }, "degrees, first swing of the settle wiggle; 0 = none" },
    { "MOTOR",      [](const Settings& s) { return (int32_t)s.motor; },           [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, (int32_t)LifterMotor::Count - 1)) return false; s.motor = (uint8_t)v; return true; }, "0 Pololu 6.3:1, 1 Pololu 19:1, 2 IA-Parts" },
    { "MINUP",      [](const Settings& s) { return (int32_t)s.minPowerUpPct; },   [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 100)) return false; s.minPowerUpPct = (uint8_t)v; return true; }, "% lowest lifter power going up; 0 = calibrated/profile" },
    { "MINDOWN",    [](const Settings& s) { return (int32_t)s.minPowerDownPct; }, [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 100)) return false; s.minPowerDownPct = (uint8_t)v; return true; }, "% lowest lifter power going down" },
    { "APPROACH",   [](const Settings& s) { return (int32_t)s.approachPowerPct; },[](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 100)) return false; s.approachPowerPct = (uint8_t)v; return true; }, "% power on the final approach to a limit; 0 = profile" },
    { "EXPERT",     [](const Settings& s) { return (int32_t)s.expertMode; },      [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.expertMode = (uint8_t)v; return true; }, "0/1 lift the 19:1 throttle caps" },
    { "DRIFT",      [](const Settings& s) { return (int32_t)s.driftPct; },        [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 20)) return false; s.driftPct = (uint8_t)v; return true; }, "% drift before the hold re-seeks; 0 = off" },
    { "ROTARYMIN",  [](const Settings& s) { return (int32_t)s.rotaryMinPct; },    [](Settings& s, int32_t v) { if (!setkey::inRange(v, 10, 100)) return false; s.rotaryMinPct = (uint8_t)v; return true; }, "% floor for rotary moves" },
    { "ROTARYOFF",  [](const Settings& s) { return (int32_t)s.rotaryDisabled; },  [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.rotaryDisabled = (uint8_t)v; return true; }, "0/1 no rotary fitted" },
    { "DRIVESIGN",  [](const Settings& s) { return (int32_t)s.driveSign; },       [](Settings& s, int32_t v) { if (v != 1 && v != -1) return false; s.driveSign = (int8_t)v; return true; }, "-1 (v3 wiring) or 1" },
    { "AGGRESSION", [](const Settings& s) { return (int32_t)s.aggressiveness; },  [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 2)) return false; s.aggressiveness = (uint8_t)v; return true; }, "random mode level: 0 gentle, 1 medium, 2 aggressive" },
    { "ID",         [](const Settings& s) { return (int32_t)s.marcId; },          [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 255)) return false; s.marcId = (uint8_t)v; return true; }, "Marcduino id; :P<id>P100 with another id is ignored" },
    { "BAUD",       [](const Settings& s) { return (int32_t)s.cmdBaud; },         [](Settings& s, int32_t v) { if (!setkey::inRange(v, 1200, 921600)) return false; s.cmdBaud = (uint32_t)v; return true; }, "command UART baud" },
    { "DEDUP",      [](const Settings& s) { return (int32_t)s.dedupMs; },         [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 5000)) return false; s.dedupMs = (uint16_t)v; return true; }, "ms window for duplicate lines across transports; 0 = off" },
    { "DEBUG",      [](const Settings& s) { return (int32_t)s.debug; },           [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.debug = (uint8_t)v; return true; }, "0/1 verbose motion tracing" },
    { "CMDTX",      [](const Settings& s) { return (int32_t)s.cmdTx; },           [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.cmdTx = (uint8_t)v; return true; }, "0/1 echo responses on the command UART TX" },
    { "LIFTERNC",   [](const Settings& s) { return (int32_t)s.lifterLimitsNC; },  [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.lifterLimitsNC = (uint8_t)v; return true; }, "0 limit switches normally open, 1 normally closed (after #PRESTART)" },
    { "ROTARYNC",   [](const Settings& s) { return (int32_t)s.rotaryHomeNC; },    [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 1)) return false; s.rotaryHomeNC = (uint8_t)v; return true; }, "0 home switch normally open, 1 normally closed (after #PRESTART)" },
    { "RANDOMLIGHT", [](const Settings& s) { return (int32_t)s.randomLight; },   [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 8)) return false; s.randomLight = (uint8_t)v; return true; }, "light kit in random mode: 0 leave alone, 1-7 that mode, 8 a new one each action; off when it ends" },
    { "COMMISSIONED", [](const Settings& s) { return (int32_t)s.commissioned; },  [](Settings& s, int32_t v) { if (!setkey::inRange(v, 0, 2)) return false; s.commissioned = (uint8_t)v; return true; }, "0 locked, 1 bench unlock, 2 commissioned" },
};
constexpr int kSettingKeyCount = static_cast<int>(sizeof(kSettingKeys) / sizeof(kSettingKeys[0]));

inline const SettingKey* findSettingKey(const char* name, size_t len) {
    for (int i = 0; i < kSettingKeyCount; ++i) {
        const char* k = kSettingKeys[i].name;
        if (std::strlen(k) == len && std::strncmp(k, name, len) == 0) return &kSettingKeys[i];
    }
    return nullptr;
}

// Formats one replayable line: "#PKEY=VALUE".
inline int formatSetting(const Settings& s, const SettingKey& k, char* buf, size_t n) {
    return std::snprintf(buf, n, "#P%s=%ld", k.name, static_cast<long>(k.get(s)));
}

// ---------------------------------------------------------------- v3 import

// What v3 kept in NVS namespace "uppityspinner" (docs/v3-inventory §6.1). -1 = key absent.
struct V3Prefs {
    bool present = false;
    int32_t lftminpwr = -1, lftseekbotpwr = -1, rotminpwr = -1, lftdist = -1, minheight = -1;
    int32_t rotenccount = -1, driftpct = -1, aggression = -1, motortype = -1, expertmode = -1, wizstate = -1;
};

struct ImportReport {
    static constexpr int kMax = 12;
    char line[kMax][96];
    int n = 0;
    int taken = 0;
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
inline void ImportReport::add(const char* fmt, ...) {
    if (n >= kMax) return;
    va_list ap; va_start(ap, fmt); std::vsnprintf(line[n], sizeof line[n], fmt, ap); va_end(ap); ++n;
}

// One-shot, conservative: a v3 value is taken only when it is clearly the builder's own
// number, never when it is v3's default (a default is a guess about somebody else's hardware:
// v3's minheight default of half travel is what let Todd's head hit the lifter frame).
inline void importV3(Settings& s, const V3Prefs& v, ImportReport& rep) {
    rep.n = 0; rep.taken = 0;
    if (!v.present) { rep.add("V3 import: no v3 settings in flash (nothing to take)"); s.v3Imported = 1; return; }
    uint8_t motor = s.motor;
    if (v.motortype >= 0 && v.motortype < static_cast<int32_t>(LifterMotor::Count)) { motor = static_cast<uint8_t>(v.motortype); s.motor = motor; rep.add("V3 import: motor profile %d (%s)", (int)motor, profileFor(static_cast<LifterMotor>(motor)).name); ++rep.taken; }
    else rep.add("V3 import: motortype absent, keeping profile %d", (int)motor);
    const LifterMotorProfile& prof = profileFor(static_cast<LifterMotor>(motor));
    if (v.lftminpwr >= 1 && v.lftminpwr <= 100) {
        if (v.lftminpwr != prof.defaultMinPowerPct) { s.minPowerUpPct = s.minPowerDownPct = static_cast<uint8_t>(v.lftminpwr); rep.add("V3 import: lifter min power %ld%% (up and down)", (long)v.lftminpwr); ++rep.taken; }
        else rep.add("V3 import: lifter min power %ld%% is the profile default, skipped", (long)v.lftminpwr);
    }
    if (v.lftseekbotpwr >= 1 && v.lftseekbotpwr <= 100) {
        if (v.lftseekbotpwr != prof.defaultSeekBottomPowerPct) { s.approachPowerPct = static_cast<uint8_t>(v.lftseekbotpwr); rep.add("V3 import: approach power %ld%%", (long)v.lftseekbotpwr); ++rep.taken; }
        else rep.add("V3 import: approach power %ld%% is the profile default, skipped", (long)v.lftseekbotpwr);
    }
    if (v.rotminpwr >= 10 && v.rotminpwr <= 100) {
        if (v.rotminpwr != 40) { s.rotaryMinPct = static_cast<uint8_t>(v.rotminpwr); rep.add("V3 import: rotary min power %ld%%", (long)v.rotminpwr); ++rep.taken; }
        else rep.add("V3 import: rotary min power 40%% is the default, skipped");
    }
    bool travelTaken = false;
    if (v.lftdist >= 100 && v.lftdist <= 5000) {
        if (v.lftdist != prof.defaultTravelTicks) { s.travelTicks = static_cast<uint16_t>(v.lftdist); travelTaken = true; rep.add("V3 import: travel %ld ticks", (long)v.lftdist); ++rep.taken; }
        else rep.add("V3 import: travel %ld is the profile guess (v3 never measured it), skipped: send #PTRAVEL<ticks> or run #PSC", (long)v.lftdist);
    }
    if (v.minheight > 0 && v.minheight <= 5000) {
        bool isDefault = v.lftdist > 0 && v.minheight == v.lftdist / 2;
        bool tooHigh = v.lftdist > 0 && v.minheight >= v.lftdist;
        if (isDefault) rep.add("V3 import: minheight %ld is v3's half-travel default, NOT taken (only the top counts as clear until #PSAFESPIN)", (long)v.minheight);
        else if (tooHigh) rep.add("V3 import: minheight %ld is not below travel, skipped", (long)v.minheight);
        else { s.safeSpinTicks = static_cast<uint16_t>(v.minheight); rep.add("V3 import: safe-spin height %ld ticks above the bottom switch (v3 minheight)%s", (long)v.minheight, travelTaken ? "" : "; confirm by eye before the first rotation below the top"); ++rep.taken; }
    }
    if (v.rotenccount >= 100 && v.rotenccount <= 5000) { s.ticksPerRev = static_cast<float>(v.rotenccount); s.tprConfident = 0; rep.add("V3 import: rotary %ld ticks/rev (v3's figure; #PROTARYTEST3 or #PROTARYTPR<tenths> to refine)", (long)v.rotenccount); ++rep.taken; }
    if (v.driftpct >= 0 && v.driftpct <= 20 && v.driftpct != 5) { s.driftPct = static_cast<uint8_t>(v.driftpct); rep.add("V3 import: drift %ld%%", (long)v.driftpct); ++rep.taken; }
    if (v.aggression >= 0 && v.aggression <= 2 && v.aggression != 1) { s.aggressiveness = static_cast<uint8_t>(v.aggression); rep.add("V3 import: aggressiveness %ld", (long)v.aggression); ++rep.taken; }
    if (v.expertmode == 1) { s.expertMode = 1; rep.add("V3 import: expert mode on"); ++rep.taken; }
    if (v.wizstate >= 0) rep.add("V3 import: wizard state %ld noted; v4 stays LOCKED until #PUNLOCK (or commissioning, phase 4)", (long)v.wizstate);
    s.v3Imported = 1;
    sanitize(s);
}

} // namespace uppity
