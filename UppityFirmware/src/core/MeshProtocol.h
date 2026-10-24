#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Units.h"
#include "Parser.h"
#include "StatusSnapshot.h"
#include "FaultModel.h"
#include "Knowledge.h"
#include "LineAssembler.h"
// The periscope's side of the WCB mesh (docs/REWRITE-PLAN.md Appendix C.2), as pure decisions:
// what an inbound message is, which "#P" lines the mesh may run, the "&PERI,..." lines it sends,
// and when it sends them. Mesh.cpp only moves bytes between the radio and these functions.
//
//   &PERI,HB,<fw>,<uptimeS>,<state>,<height%>,<rotDeg>,<homed>  broadcast every hbPeriodS (10 s default)
//   &PERI,POS,<height%>,<rotDeg>                                 to Sabé (special peer 20) on change, <= 1 Hz
//   &PERI,STATE,<state>                                          broadcast on a transition
//   &PERI,FAULT,<name>                                           to Sabé, ensured; broadcast if that fails
//   &PERI,ACK,<verb> / &PERI,NAK,<verb>,<reason>                 broadcast, only for the verbs below
//   &PERI,HELLO,<fw>,<proto>                                     broadcast, the answer to &HELLO
//
// Unknown height or angle is -1. <homed> is 1 when the head is at home (lined up for the dome).
// Inbound: ":P"/"#P" lines (the dispatcher decides whose), &ESTOP, &ESTOP,RESET, &DISABLE,
// &STATUS, &HELLO, and "?STOP". Everything else gets no answer: replying to other boards'
// telemetry was the largest source of mesh junk on Todd's droid (Sabé, 2026-08-17).
namespace uppity {

constexpr int    kMeshProto = 1;
constexpr size_t kMeshPacketMax = 187;   // one ESP-NOW packet with the checksum on; broadcasts never fragment
constexpr uint8_t kMeshControllerId = 20;   // the special peer: Sabé on Todd's droid
constexpr Ms     kMeshPosMinMs = 1000;
constexpr Ms     kMeshFirstHbMs = 3000;  // after begin(): the library's own boot announces go first

enum class MeshMsg : uint8_t { Foreign = 0, Line, Estop, EstopReset, Disable, Status, Hello, StopAll };

namespace meshdetail {
// "&VERB" exactly, ignoring trailing spaces / CR / LF
inline bool isExactly(const char* p, const char* verb) {
    size_t n = std::strlen(verb);
    if (std::strncmp(p, verb, n) != 0) return false;
    for (p += n; *p; ++p) if (*p != ' ' && *p != '\r' && *p != '\n') return false;
    return true;
}
inline bool startsWith(const char* p, const char* s) { return std::strncmp(p, s, std::strlen(s)) == 0; }
}

inline MeshMsg classifyMesh(const char* text) {
    if (!text) return MeshMsg::Foreign;
    const char* p = text;
    while (*p == ' ') ++p;
    if (*p == ':' || *p == '#') return MeshMsg::Line;           // the dispatcher drops other boards' lines silently
    if (meshdetail::isExactly(p, "?STOP")) return MeshMsg::StopAll;
    if (*p != '&') return MeshMsg::Foreign;
    if (meshdetail::isExactly(p, "&ESTOP,RESET")) return MeshMsg::EstopReset;
    if (meshdetail::isExactly(p, "&ESTOP") || (meshdetail::startsWith(p, "&ESTOP,") && !meshdetail::startsWith(p, "&ESTOP,RESET"))) return MeshMsg::Estop;
    if (meshdetail::isExactly(p, "&DISABLE")) return MeshMsg::Disable;
    if (meshdetail::isExactly(p, "&STATUS")) return MeshMsg::Status;
    if (meshdetail::isExactly(p, "&HELLO")) return MeshMsg::Hello;
    return MeshMsg::Foreign;                                     // &SABE,…, &RAD,…, our own &PERI echoes, anything new
}

// Does this mesh message latch the E-STOP? The same patterns as the serial ports (":PX", "?STOP",
// "&ESTOP", "&SABE,ESTOP"; EstopScanner), plus "&ESTOP,<reason>". Called in the radio task.
inline bool meshWantsEstop(const char* text) {
    if (!text) return false;
    EstopScanner scan;
    bool trip = false;
    for (const char* p = text; *p; ++p) trip |= scan.feed(*p);
    trip |= scan.feed('\n');                     // one message is one line
    MeshMsg m = classifyMesh(text);
    return trip || m == MeshMsg::Estop || m == MeshMsg::StopAll;
}

// One mesh message is one line (the brief: the serial ports' rules). Copies it without the trailing
// CR/LF/spaces; false (drop it whole) when it does not fit `cap` or holds any other control
// character: WCB_Client reassembles unicasts of up to ~2.8 KB, and a cut line would still run
// (":PP100" read as ":PP10"), while a line break inside one would reach the serial bus as a second
// line when the log is echoed there (review of the mesh, 2026-10-05).
inline bool meshCleanLine(const char* in, char* out, size_t cap) {
    if (!in || cap == 0) return false;
    size_t n = std::strlen(in);
    while (n > 0 && (in[n - 1] == '\r' || in[n - 1] == '\n' || in[n - 1] == ' ')) --n;
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; ++i) { unsigned char c = static_cast<unsigned char>(in[i]); if (c < 0x20 || c == 0x7f) return false; }
    std::memcpy(out, in, n); out[n] = '\0';
    return true;
}

// The mesh is unattended and over the air: it may move the periscope (":P", through the same
// policy as every source, never with the rescue override or the commissioning unlock), ask for
// state, and release an E-STOP (#PRESUME, the same as &ESTOP,RESET). Settings, commissioning,
// unlocking, stored sequences, restarts and factory resets come from the USB console or the page.
// The questions that read flash (#PCONFIG lists the stored sequences, #PL, #PV3) are not taken
// from it either: only Persist touches NVS, and never while a motor may be running (hard rule 5).
inline bool meshMayRunConfig(Verb v) {
    switch (v) {
        case Verb::State: case Verb::Status: case Verb::Cal: case Verb::Help: case Verb::Resume:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------- what the periscope reports

struct PeriStatus {
    const char* state = "LOCKED";
    int heightPct = -1;          // -1 unknown
    int rotDeg = -1;             // -1 unknown
    int homed = 0;
    uint32_t faults = 0;
};

inline const char* meshStateName(const Snapshot& s) {
    if (s.estop) return "ESTOP";
    switch (s.state) {
        case MotionState::Locked: return "LOCKED";
        case MotionState::Idle: return "IDLE";
        case MotionState::Moving: return "MOVING";
        case MotionState::Stopped: return "ESTOP";
        case MotionState::Fault: return "FAULT";
        default: return "IDLE";
    }
}

inline PeriStatus periStatus(const Snapshot& s) {
    PeriStatus p;
    p.state = meshStateName(s);
    if (s.lifterConf == LifterConfidence::Known) p.heightPct = s.lifterPct < 0 ? 0 : s.lifterPct > 100 ? 100 : s.lifterPct;
    bool homeKnown = s.homeConf == HomeConfidence::SwitchLive || s.homeConf == HomeConfidence::Latched;
    if (!s.rotaryDisabled && homeKnown && s.ticksPerRev > 0.f) p.rotDeg = ((s.rotaryDeg % 360) + 360) % 360;
    if (s.rotaryDisabled) p.homed = 1;
    else if (p.rotDeg >= 0) p.homed = (p.rotDeg <= 10 || p.rotDeg >= 350) ? 1 : 0;
    p.faults = s.faults;
    return p;
}

inline const char* faultName(uint32_t bit) {
    switch (bit) {
        case F_LifterDiag: return "LIFTER_DRIVER"; case F_RotaryDiag: return "ROTARY_DRIVER"; case F_DiagRepeated: return "DRIVER_REPEATED";
        case F_LifterStall: return "LIFTER_STALL"; case F_RotaryStall: return "ROTARY_STALL"; case F_EncoderDirection: return "ENCODER_DIRECTION";
        case F_EncoderNoise: return "ENCODER_NOISE"; case F_LimitContradiction: return "LIMIT_CONTRADICTION"; case F_LimitMismatch: return "LIMIT_MISMATCH";
        case F_HomeSwitchStuck: return "HOME_SWITCH_STUCK"; case F_Expander: return "EXPANDER"; case F_TickOverrun: return "TICK_OVERRUN";
        case F_PersistDeferred: return "SAVE_DEFERRED";
        default: return "FAULT";
    }
}

// Each returns the length written (snprintf), or -1 when it would not fit one packet.
namespace meshfmt {
inline int fit(int n, size_t cap) { return (n < 0 || static_cast<size_t>(n) >= cap || static_cast<size_t>(n) > kMeshPacketMax) ? -1 : n; }
}
inline int formatMeshHb(char* b, size_t n, const char* fw, uint32_t uptimeS, const PeriStatus& p) {
    return meshfmt::fit(std::snprintf(b, n, "&PERI,HB,%s,%lu,%s,%d,%d,%d", fw, static_cast<unsigned long>(uptimeS), p.state, p.heightPct, p.rotDeg, p.homed), n);
}
inline int formatMeshPos(char* b, size_t n, const PeriStatus& p) { return meshfmt::fit(std::snprintf(b, n, "&PERI,POS,%d,%d", p.heightPct, p.rotDeg), n); }
inline int formatMeshState(char* b, size_t n, const PeriStatus& p) { return meshfmt::fit(std::snprintf(b, n, "&PERI,STATE,%s", p.state), n); }
inline int formatMeshFault(char* b, size_t n, uint32_t bit) { return meshfmt::fit(std::snprintf(b, n, "&PERI,FAULT,%s", faultName(bit)), n); }
inline int formatMeshHello(char* b, size_t n, const char* fw) { return meshfmt::fit(std::snprintf(b, n, "&PERI,HELLO,%s,%d", fw, kMeshProto), n); }
inline int formatMeshAck(char* b, size_t n, MeshMsg m) {
    const char* v = m == MeshMsg::Estop ? "ESTOP" : m == MeshMsg::EstopReset ? "ESTOP.RESET" : m == MeshMsg::Disable ? "DISABLE" : m == MeshMsg::Status ? "STATUS" : nullptr;
    if (!v) return -1;
    return meshfmt::fit(std::snprintf(b, n, "&PERI,ACK,%s", v), n);
}
inline int formatMeshNak(char* b, size_t n, MeshMsg m, const char* reason) {
    const char* v = m == MeshMsg::EstopReset ? "ESTOP.RESET" : m == MeshMsg::Disable ? "DISABLE" : nullptr;
    if (!v) return -1;
    return meshfmt::fit(std::snprintf(b, n, "&PERI,NAK,%s,%s", v, reason), n);
}

// When to send what. tick() runs from loop(); `out` is the radio (or a test's recorder).
class MeshReporter {
  public:
    struct Out {
        virtual ~Out() = default;
        virtual bool broadcast(const char* text) = 0;
        virtual bool toController(const char* text, bool ensured) = 0;   // false: not sent (no controller, or it failed)
    };
    void start(Ms now, uint16_t hbPeriodS) {
        periodMs_ = static_cast<Ms>(hbPeriodS < 5 ? 5 : hbPeriodS > 300 ? 300 : hbPeriodS) * 1000;
        nextHb_ = now + kMeshFirstHbMs; started_ = true; havePos_ = false; haveState_ = false; lastFaults_ = 0; lastPosMs_ = now;
    }
    void tick(const Snapshot& s, Ms now, uint32_t uptimeS, const char* fw, Out& out) {
        if (!started_) return;
        PeriStatus p = periStatus(s);
        char b[kMeshPacketMax + 1];
        if (static_cast<int32_t>(now - nextHb_) >= 0) {
            if (formatMeshHb(b, sizeof b, fw, uptimeS, p) > 0) { out.broadcast(b); ++hbSent_; lastHbMs_ = now; }
            nextHb_ = now + periodMs_;
        }
        if (!haveState_ || std::strcmp(p.state, lastState_) != 0) {
            // the first state is in the first heartbeat; transitions after that get their own line
            if (haveState_ && formatMeshState(b, sizeof b, p) > 0) out.broadcast(b);
            std::snprintf(lastState_, sizeof lastState_, "%s", p.state); haveState_ = true;
        }
        if ((!havePos_ || p.heightPct != lastH_ || p.rotDeg != lastR_) && elapsed(now, lastPosMs_) >= kMeshPosMinMs) {
            if (formatMeshPos(b, sizeof b, p) > 0) out.toController(b, false);
            lastH_ = p.heightPct; lastR_ = p.rotDeg; havePos_ = true; lastPosMs_ = now;
        }
        uint32_t fresh = p.faults & ~lastFaults_;
        for (uint32_t bit = 1; fresh && bit; bit <<= 1) {
            if (!(fresh & bit)) continue;
            fresh &= ~bit;
            if (formatMeshFault(b, sizeof b, bit) > 0 && !out.toController(b, true)) out.broadcast(b);   // ensured to Sabé, else everyone
        }
        lastFaults_ = p.faults;
    }
    // &STATUS: everything now (heartbeat, state, position), then the schedule goes on.
    void burst(const Snapshot& s, Ms now, uint32_t uptimeS, const char* fw, Out& out) {
        PeriStatus p = periStatus(s);
        char b[kMeshPacketMax + 1];
        if (formatMeshHb(b, sizeof b, fw, uptimeS, p) > 0) { out.broadcast(b); ++hbSent_; lastHbMs_ = now; }
        if (formatMeshState(b, sizeof b, p) > 0) out.broadcast(b);
        if (formatMeshPos(b, sizeof b, p) > 0) out.toController(b, false);
        lastH_ = p.heightPct; lastR_ = p.rotDeg; havePos_ = true; lastPosMs_ = now;
        std::snprintf(lastState_, sizeof lastState_, "%s", p.state); haveState_ = true;
    }
    uint32_t heartbeatsSent() const { return hbSent_; }
    Ms lastHeartbeatMs() const { return lastHbMs_; }
  private:
    Ms periodMs_ = 10000, nextHb_ = 0, lastPosMs_ = 0, lastHbMs_ = 0;
    bool started_ = false, havePos_ = false, haveState_ = false;
    int lastH_ = -2, lastR_ = -2;
    char lastState_[12] = {};
    uint32_t lastFaults_ = 0, hbSent_ = 0;
};

// Broadcasts are not acknowledged on the air: the three lines of a &STATUS answer, sent in the same
// millisecond, reached a WCB on 7.0.1 as two (bench 2026-10-24, the middle one lost). The board's
// own broadcasts leave in order and at least kMeshTxGapMs apart; a full queue drops the newest
// (telemetry: the next heartbeat or state line supersedes it). Unicasts to Sabé are acknowledged
// and retried by the radio, so they do not wait here.
constexpr Ms kMeshTxGapMs = 40;
class MeshTxPacer {
  public:
    static constexpr int kSlots = 6;
    bool push(const char* text) {
        if (!text || count_ == kSlots) { ++dropped_; return false; }
        std::snprintf(slot_[(head_ + count_) % kSlots], kMeshPacketMax + 1, "%s", text);
        ++count_;
        return true;
    }
    // Sends the oldest line once the gap since the last one has passed; at most one per call.
    template <class Send> void service(Ms now, Send&& send) {
        if (count_ == 0) return;
        if (sentAny_ && elapsed(now, lastMs_) < kMeshTxGapMs) return;
        send(static_cast<const char*>(slot_[head_]));
        head_ = (head_ + 1) % kSlots; --count_;
        lastMs_ = now; sentAny_ = true;
    }
    int pending() const { return count_; }
    uint32_t dropped() const { return dropped_; }
  private:
    char slot_[kSlots][kMeshPacketMax + 1] = {};
    int head_ = 0, count_ = 0;
    Ms lastMs_ = 0;
    bool sentAny_ = false;
    uint32_t dropped_ = 0;
};

} // namespace uppity
