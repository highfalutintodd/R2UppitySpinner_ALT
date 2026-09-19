#pragma once
#include <cstdio>
#include "EventLog.h"
#include "StatusSnapshot.h"
#include "Knowledge.h"
// Turns coded events into one-line text. Pure; used by the console log sink and by tests.
namespace uppity {

inline const char* jobName(int k) {
    switch (static_cast<JobKind>(k)) {
        case JobKind::LifterSeek: return "SEEK"; case JobKind::LifterCreep: return "CREEP"; case JobKind::LifterCalibrate: return "CALIBRATE";
        case JobKind::LifterHold: return "HOLD"; case JobKind::RotaryAbsolute: return "ROT_ABS"; case JobKind::RotaryRelative: return "ROT_REL";
        case JobKind::RotarySpin: return "SPIN"; case JobKind::RotaryJog: return "ROT_JOG"; case JobKind::RotaryHome: return "HOME";
        case JobKind::RotaryCharacterise: return "ROTARYTEST"; case JobKind::SafeLower: return "SAFE_LOWER"; default: return "NONE";
    }
}
inline const char* resultName(int r) {
    switch (static_cast<JobResult>(r)) {
        case JobResult::Running: return "RUNNING"; case JobResult::Done: return "DONE"; case JobResult::DoneLimit: return "DONE_LIMIT";
        case JobResult::DoneTarget: return "DONE_TARGET"; case JobResult::Clamped: return "CLAMPED"; case JobResult::Refused: return "REFUSED";
        case JobResult::Aborted: return "ABORTED"; case JobResult::Stalled: return "STALLED"; case JobResult::Failed: return "FAILED";
        case JobResult::FaultExit: return "FAULT"; default: return "NONE";
    }
}
inline const char* reasonName(int r) {
    switch (static_cast<Reason>(r)) {
        case Reason::Estop: return "ESTOP"; case Reason::Cancel: return "CANCEL"; case Reason::LimitReached: return "LIMIT"; case Reason::TargetReached: return "TARGET";
        case Reason::Stall: return "STALL"; case Reason::SlowProgress: return "SLOW_PROGRESS"; case Reason::DiagFault: return "DRIVER_FAULT"; case Reason::Timeout: return "TIMEOUT";
        case Reason::RotaryNotHome: return "ROTARY_NOT_HOME"; case Reason::LifterUnknown: return "LIFTER_UNKNOWN"; case Reason::BelowSafeHeight: return "BELOW_SAFE_HEIGHT";
        case Reason::Locked: return "LOCKED"; case Reason::LeaseExpired: return "LEASE_EXPIRED"; case Reason::ExpanderFault: return "EXPANDER_FAULT"; case Reason::NoEdge: return "NO_EDGE";
        case Reason::SwitchStuck: return "SWITCH_STUCK"; case Reason::NotRepeatable: return "NOT_REPEATABLE"; case Reason::PolicyChange: return "POLICY"; case Reason::Overshoot: return "OVERSHOOT";
        case Reason::AlreadyThere: return "ALREADY_THERE"; case Reason::HomeFailed: return "HOME_FAILED"; case Reason::EncoderDirection: return "ENCODER_DIRECTION";
        case Reason::LimitContradiction: return "LIMIT_CONTRADICTION"; case Reason::Busy: return "BUSY"; case Reason::RotaryDisabled: return "ROTARY_DISABLED";
        case Reason::EnableTimeout: return "ENABLE_TIMEOUT"; case Reason::Breakaway: return "BREAKAWAY"; case Reason::AlreadyHome: return "ALREADY_HOME";
        case Reason::Skipped: return "SKIPPED"; case Reason::HoldGaveUp: return "HOLD_GAVE_UP"; default: return "-";
    }
}
inline const char* stateName(int s) {
    switch (static_cast<MotionState>(s)) { case MotionState::Locked: return "LOCKED"; case MotionState::Idle: return "IDLE"; case MotionState::Moving: return "MOVING"; case MotionState::Stopped: return "STOPPED"; case MotionState::Fault: return "FAULT"; default: return "?"; }
}
inline const char* lifterConfName(int c) { switch (static_cast<LifterConfidence>(c)) { case LifterConfidence::Known: return "KNOWN"; case LifterConfidence::Stale: return "STALE"; default: return "UNKNOWN"; } }
inline const char* homeConfName(int c) { switch (static_cast<HomeConfidence>(c)) { case HomeConfidence::SwitchLive: return "SWITCH_LIVE"; case HomeConfidence::Latched: return "LATCHED"; case HomeConfidence::Stale: return "STALE"; default: return "UNKNOWN"; } }

// Writes "t=12.345 EVENT detail" without a newline. Returns chars written.
inline int formatEvent(const Event& e, char* buf, size_t n) {
    char head[24]; std::snprintf(head, sizeof head, "t=%lu.%03lu ", static_cast<unsigned long>(e.tMs / 1000), static_cast<unsigned long>(e.tMs % 1000));
    switch (e.code) {
        case Ev::Boot: return std::snprintf(buf, n, "%sBOOT lifter=%s rotary=%s limits=[top=%d bot=%d home=%d]", head, lifterConfName(e.a), homeConfName(e.b), (e.c & 1) != 0, (e.c & 2) != 0, (e.c & 4) != 0);
        case Ev::StateChange: return std::snprintf(buf, n, "%sSTATE %s -> %s", head, stateName(e.a), stateName(e.b));
        case Ev::JobStart: return std::snprintf(buf, n, "%s%s START param=%ld id=%ld", head, jobName(e.a), (long)e.b, (long)e.c);
        case Ev::JobEnd: return std::snprintf(buf, n, "%s%s %s %s", head, jobName(e.a), resultName(e.b), reasonName(e.c));
        case Ev::SeekClamped: return std::snprintf(buf, n, "%sSEEK CLAMPED %ld -> %ld reason=%s", head, (long)e.a, (long)e.b, reasonName(e.c));
        case Ev::LimitTop: return std::snprintf(buf, n, "%sLIMIT TOP %s at %ld", head, e.a ? "made" : "released", (long)e.b);
        case Ev::LimitBottom: return std::snprintf(buf, n, "%sLIMIT BOTTOM %s at %ld", head, e.a ? "made" : "released", (long)e.b);
        case Ev::HomeSwitch: return std::snprintf(buf, n, "%sHOME SWITCH %s at %ld", head, e.a ? "closed" : "open", (long)e.b);
        case Ev::HomeEdge: return std::snprintf(buf, n, "%sHOME EDGE enter=%ld exit=%ld closures=%ld", head, (long)e.a, (long)e.b, (long)e.c);
        case Ev::HomeFrameCorrected: return std::snprintf(buf, n, "%sHOME FRAME CORRECTED by %ld ticks (switch wins)", head, (long)e.a);
        case Ev::ConfidenceLifter: return std::snprintf(buf, n, "%sLIFTER %s -> %s", head, lifterConfName(e.a), lifterConfName(e.b));
        case Ev::ConfidenceRotary: return std::snprintf(buf, n, "%sROTARY %s -> %s", head, homeConfName(e.a), homeConfName(e.b));
        case Ev::Estop: return std::snprintf(buf, n, "%sESTOP %s (source %ld)", head, e.b ? "LATCHED" : "released", (long)e.a);
        case Ev::Fault: return std::snprintf(buf, n, "%sFAULT mask=0x%lx %s%s", head, (unsigned long)e.a, e.b ? "set" : "cleared", e.c == 1 ? " (EN cycle)" : e.c == 2 ? " (persists after EN cycle)" : "");
        case Ev::Enable: return std::snprintf(buf, n, "%sDRIVERS %s", head, e.a ? "enable" : "disable");
        case Ev::Phase: return std::snprintf(buf, n, "%s%s phase %ld at %ld", head, jobName(e.a), (long)e.b, (long)e.c);
        case Ev::Measure:
            if (e.a == 1) return std::snprintf(buf, n, "%sROTARY ticks/rev=%ld.%ld spread=%ld.%ld%%", head, (long)e.b / 10, (long)e.b % 10, (long)e.c / 10, (long)e.c % 10);
            if (e.a == 2) return std::snprintf(buf, n, "%sROTARY contact arc=%ld ticks closures=%ld", head, (long)e.b, (long)e.c);
            if (e.a == 3) return std::snprintf(buf, n, "%sCAL travel=%ld ticks spread=%ld.%ld%%", head, (long)e.b, (long)e.c / 10, (long)e.c % 10);
            if (e.a == 4) return std::snprintf(buf, n, "%sCAL min power up=%ld down=%ld (permille)", head, (long)e.b, (long)e.c);
            return std::snprintf(buf, n, "%sMEASURE %ld %ld %ld", head, (long)e.a, (long)e.b, (long)e.c);
        case Ev::Refused: return std::snprintf(buf, n, "%s%s REFUSED %s", head, jobName(e.a), reasonName(e.b));
        case Ev::Override: return std::snprintf(buf, n, "%sOVERRIDE %s", head, e.a ? "on" : "off");
        case Ev::LightKit: return std::snprintf(buf, n, "%sLIGHTKIT %ld", head, (long)e.a);
        case Ev::Calibration: return std::snprintf(buf, n, "%sCAL phase %ld at %ld", head, (long)e.a, (long)e.b);
        case Ev::Hold: return e.a ? std::snprintf(buf, n, "%sHOLD re-seek target=%ld from %ld", head, (long)e.b, (long)e.c) : std::snprintf(buf, n, "%sHOLD gave up target=%ld at %ld", head, (long)e.b, (long)e.c);
        case Ev::Tick: return std::snprintf(buf, n, "%sTICK cycleUsMax=%ld late=%ld", head, (long)e.a, (long)e.b);
        default: return std::snprintf(buf, n, "%sEV %d %ld %ld %ld", head, (int)e.code, (long)e.a, (long)e.b, (long)e.c);
    }
}

} // namespace uppity
