#pragma once
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdio>
#include "Units.h"
// Marcduino grammar for the Uppity Spinner (":P..." motion, "#P..." configuration), parsed
// into Command structs. Pure. docs/REWRITE-PLAN.md Appendix A is the reference.
//   :PP<0-100>[,speed]  :PPR[,speed]  :PH[speed]  :PA<deg>[,spd][,max]  :PAR  :PD<deg>[,spd][,max]
//   :PDR  :PR<+-speed>  :PRR[max]  :PW[R]<sec>  :PL<0-7>  :PLR  :PS<n>  :PM  :PMG/M/A  :PM,lift,rot,min,max  :PX
//   #P<KEY>[=]<value> for every key in Settings.h (e.g. #PINVERT1, #PTRAVEL=1150), #P<KEY> to read
//   #PRESUME #PSTATE #PSTATUS #PROTARYTEST[n] #PSC #PCLEARFAULT #PCONFIG #PCAL #PUNLOCK #PLOCK #PHELP
//   #PS<n>:<elements> #PL #PD<n> #PSEQNAME<n>=<name> #PZERO #PFACTORY #PRESTART #PV3 #PV3IMPORT
//   #PR (toggle rotary) #PN{C|O}{L|R} (limit polarity)
namespace uppity {

enum class Verb : uint8_t {
    None = 0, Invalid,
    // motion / chain elements
    LiftTo, LiftRandom, SafeLower, RotAbs, RotAbsRandom, RotRel, RotRelRandom, Spin, SpinRandom, Estop,
    Wait, Light, LightRandom, PlaySeq, Random, RandomLevel,
    SpinStop,            // internal (random director): cancel a running spin
    // configuration
    Resume, State, Status, RotaryTest, Calibrate, ClearFault, Config, Cal, V3Dump, V3Import, Unlock, Lock, Help,
    Set,                 // #P<KEY>[=]<value> / #P<KEY>: key in Command::key, value in a when hasA
    StoreSeq, ListSeq, DelSeq, SeqName, Zero, Factory, Restart, RotaryToggle, LimitPolarity,
};

struct Command {
    Verb verb = Verb::None;
    int32_t a = 0, b = 0, c = 0, d = 0;   // per verb; -2 = "R" (random) where allowed
    bool hasA = false, hasB = false, hasC = false, hasD = false;
    int32_t id = -1;                       // Marcduino ID prefix, -1 = none
    char letter = 0;                       // RandomLevel: G/M/A; LimitPolarity: 'C'/'O' with a = 'L'/'R'
    char key[14] = {};                     // Set: the settings key
    const char* text = nullptr;            // StoreSeq / SeqName: points into the source line (not owned)
};

inline bool isMotionVerb(Verb v) {
    switch (v) {
        case Verb::LiftTo: case Verb::LiftRandom: case Verb::SafeLower: case Verb::RotAbs: case Verb::RotAbsRandom:
        case Verb::RotRel: case Verb::RotRelRandom: case Verb::Spin: case Verb::SpinRandom: case Verb::PlaySeq: return true;
        default: return false;
    }
}
inline bool isChainVerb(Verb v) { return isMotionVerb(v) || v == Verb::Wait || v == Verb::Light || v == Verb::LightRandom || v == Verb::Random || v == Verb::RandomLevel || v == Verb::Estop || v == Verb::SpinStop; }

namespace parse {
inline bool readInt(const char*& p, int32_t& v) {
    const char* s = p; bool neg = false;
    if (*p == '-' || *p == '+') { neg = *p == '-'; ++p; }
    if (!isdigit(static_cast<unsigned char>(*p))) { p = s; return false; }
    int32_t n = 0; while (isdigit(static_cast<unsigned char>(*p))) { if (n < 100000000) n = n * 10 + (*p - '0'); ++p; }
    v = neg ? -n : n; return true;
}
inline bool startsWith(const char* s, const char* pre) { return std::strncmp(s, pre, std::strlen(pre)) == 0; }
inline bool readArg(const char*& p, int32_t& v, bool& has) {
    if (*p != ',') return false;
    ++p;
    if (*p == 'R') { ++p; v = -2; has = true; return true; }
    if (readInt(p, v)) { has = true; return true; }
    has = false; return true;                                   // ",," : empty argument
}
inline void readOptArgs(const char*& p, Command& c) {
    readArg(p, c.b, c.hasB);
    readArg(p, c.c, c.hasC);
}
}

// Parses one line (no terminator). Returns false for lines that are not for us (the caller
// stays silent); true with verb Invalid for a recognised-but-malformed command.
inline bool parseLine(const char* line, Command& cmd) {
    cmd = Command{};
    const char* p = line;
    while (*p == ' ') ++p;
    if (*p != ':' && *p != '#') return false;
    bool config = *p == '#';
    ++p;
    if (*p != 'P') return false;
    ++p;
    if (isdigit(static_cast<unsigned char>(*p))) { int32_t id; parse::readInt(p, id); cmd.id = id; }
    using namespace parse;
    if (!config) {
        switch (*p) {
            case 'P': ++p;
                if (*p == 'R') { ++p; cmd.verb = Verb::LiftRandom; readOptArgs(p, cmd); return true; }
                if (!readInt(p, cmd.a)) { cmd.verb = Verb::Invalid; return true; }
                cmd.hasA = true; cmd.verb = Verb::LiftTo; readOptArgs(p, cmd); return true;
            case 'H': ++p; cmd.verb = Verb::SafeLower; if (readInt(p, cmd.a)) cmd.hasA = true; return true;
            case 'A': case 'D': {
                bool rel = *p == 'D'; ++p;
                if (*p == 'R') { ++p; cmd.verb = rel ? Verb::RotRelRandom : Verb::RotAbsRandom; readOptArgs(p, cmd); return true; }
                if (!readInt(p, cmd.a)) { cmd.verb = Verb::Invalid; return true; }
                cmd.hasA = true; cmd.verb = rel ? Verb::RotRel : Verb::RotAbs; readOptArgs(p, cmd); return true;
            }
            case 'R': ++p;
                if (*p == 'R') { ++p; cmd.verb = Verb::SpinRandom; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
                if (!readInt(p, cmd.a)) { cmd.verb = Verb::Invalid; return true; }
                cmd.hasA = true; cmd.verb = Verb::Spin; return true;
            case 'X': cmd.verb = Verb::Estop; return true;
            case 'W': ++p; cmd.verb = Verb::Wait; if (*p == 'R') { ++p; cmd.b = 1; } if (readInt(p, cmd.a)) cmd.hasA = true; return true;
            case 'L': ++p; if (*p == 'R') { cmd.verb = Verb::LightRandom; return true; } if (!readInt(p, cmd.a)) { cmd.verb = Verb::Invalid; return true; } cmd.hasA = true; cmd.verb = Verb::Light; return true;
            case 'S': ++p; if (!readInt(p, cmd.a)) { cmd.verb = Verb::Invalid; return true; } cmd.hasA = true; cmd.verb = Verb::PlaySeq; return true;
            case 'M': ++p;
                if (*p == 'G' || *p == 'M' || *p == 'A') { cmd.verb = Verb::RandomLevel; cmd.letter = *p; return true; }
                cmd.verb = Verb::Random;
                readArg(p, cmd.a, cmd.hasA); readArg(p, cmd.b, cmd.hasB); readArg(p, cmd.c, cmd.hasC); readArg(p, cmd.d, cmd.hasD);
                return true;
            default: cmd.verb = Verb::Invalid; return true;
        }
    }
    // ---- configuration: exact / explicit verbs first ----
    if (startsWith(p, "RESUME")) { cmd.verb = Verb::Resume; return true; }
    if (startsWith(p, "RESTART")) { cmd.verb = Verb::Restart; return true; }
    if (startsWith(p, "ROTARYTEST")) { p += 10; cmd.verb = Verb::RotaryTest; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "STATE")) { cmd.verb = Verb::State; return true; }
    if (startsWith(p, "STATUS")) { cmd.verb = Verb::Status; return true; }
    if (startsWith(p, "SEQNAME")) { p += 7; if (!readInt(p, cmd.a) || *p != '=') { cmd.verb = Verb::Invalid; return true; } cmd.hasA = true; cmd.text = p + 1; cmd.verb = Verb::SeqName; return true; }
    if (startsWith(p, "SC") && p[2] == '\0') { cmd.verb = Verb::Calibrate; return true; }
    if (startsWith(p, "CLEARFAULT")) { cmd.verb = Verb::ClearFault; return true; }
    if (startsWith(p, "CONFIG")) { cmd.verb = Verb::Config; return true; }
    if (startsWith(p, "CAL") && p[3] == '\0') { cmd.verb = Verb::Cal; return true; }
    if (startsWith(p, "V3IMPORT")) { cmd.verb = Verb::V3Import; return true; }
    if (startsWith(p, "V3")) { cmd.verb = Verb::V3Dump; return true; }
    if (startsWith(p, "UNLOCK")) { cmd.verb = Verb::Unlock; return true; }
    if (startsWith(p, "LOCK")) { cmd.verb = Verb::Lock; return true; }
    if (startsWith(p, "HELP")) { cmd.verb = Verb::Help; return true; }
    if (startsWith(p, "ZERO")) { cmd.verb = Verb::Zero; return true; }
    if (startsWith(p, "FACTORY")) { cmd.verb = Verb::Factory; return true; }
    // ---- generic settings key: letters, optional '=', optional value ----
    {
        const char* k = p;
        while (*p >= 'A' && *p <= 'Z') ++p;
        size_t klen = static_cast<size_t>(p - k);
        if (klen > 0 && klen < sizeof cmd.key) {
            const char* q = p;
            if (*q == '=') ++q;
            int32_t v;
            bool has = readInt(q, v);
            if (!has && *q != '\0') { /* not a plain key form; fall through to the special forms */ }
            else {
                std::memcpy(cmd.key, k, klen); cmd.key[klen] = '\0';
                // keys are validated by the caller against Settings.h; unknown single-letter
                // forms below (L, D<n>, R, N.., S<n>:) take precedence over a bad key
                bool special = (klen == 1 && (k[0] == 'L' || k[0] == 'D' || k[0] == 'R' || k[0] == 'S')) || (klen == 3 && k[0] == 'N');
                if (!special) { cmd.verb = Verb::Set; if (has) { cmd.a = v; cmd.hasA = true; } return true; }
            }
        }
        p = k;
    }
    // ---- special single-letter forms ----
    if (*p == 'L' && p[1] == '\0') { cmd.verb = Verb::ListSeq; return true; }
    if (*p == 'R' && p[1] == '\0') { cmd.verb = Verb::RotaryToggle; return true; }
    if (*p == 'D') { const char* q = p + 1; if (readInt(q, cmd.a) && *q == '\0') { cmd.hasA = true; cmd.verb = Verb::DelSeq; return true; } }
    if (*p == 'N' && (p[1] == 'C' || p[1] == 'O') && (p[2] == 'L' || p[2] == 'R') && p[3] == '\0') { cmd.verb = Verb::LimitPolarity; cmd.letter = p[1]; cmd.a = p[2]; return true; }
    if (*p == 'S') { const char* q = p + 1; if (readInt(q, cmd.a) && *q == ':') { cmd.hasA = true; cmd.text = q + 1; cmd.verb = Verb::StoreSeq; return true; } }
    cmd.verb = Verb::Invalid;
    return true;
}

// Walk a ':'-chained line element by element (":PP100:PA90" -> ":PP100", ":PA90"). Elements
// keep their leading ':' so each parses on its own. `cursor` starts at the line; returns false
// when exhausted. Copies into `out` (size n) so the source line is never modified.
inline bool nextChainElement(const char*& cursor, char* out, size_t n) {
    if (!cursor || *cursor == '\0' || n == 0) return false;
    const char* start = cursor;
    const char* q = start + 1;
    while (*q && *q != ':') ++q;
    size_t len = static_cast<size_t>(q - start);
    if (len >= n) len = n - 1;
    std::memcpy(out, start, len); out[len] = '\0';
    cursor = q;
    return true;
}

// Bring one chain element into the full ":P..." form. Accepts ":PP100" (full), "PP100" (full
// without the colon), "P100" / "L5" / "W2" (v3's legacy stored-sequence elements) and a bare
// "X". Returns false if the element is empty.
inline bool normaliseElement(const char* el, char* out, size_t n) {
    while (*el == ':') ++el;
    if (*el == '\0') return false;
    // "PP100", "PL5", "PH", "P3P50" (with an id) are full form; "P100", "L5", "W2", "X" are legacy
    const char* q = el + 1;
    if (el[0] == 'P') { while (*q >= '0' && *q <= '9') ++q; }
    bool full = el[0] == 'P' && *q >= 'A' && *q <= 'Z';
    std::snprintf(out, n, full ? ":%s" : ":P%s", el);
    return true;
}

// Validate a stored-sequence body ("P100:L5:R30", ":PP100:PL5:PR30" or "PP100:PL5") and write
// its canonical form (":PP100:PL5:PR30") with waits clamped to 60 s. Returns the number of
// elements, 0 if empty, -1 with the offending element copied into `bad` if one is invalid.
inline int canonicalSequence(const char* body, char* out, size_t n, char* bad, size_t badN) {
    if (n == 0) return -1;
    out[0] = '\0'; if (badN) bad[0] = '\0';
    size_t used = 0; int count = 0;
    const char* cursor = body;
    char el[64], norm[72];
    // the body may open with ':' (full form) — nextChainElement wants each element to start at
    // its ':' so step over a leading run of separators first
    while (*cursor == ':') ++cursor;
    while (*cursor) {
        const char* q = cursor; while (*q && *q != ':') ++q;
        size_t len = static_cast<size_t>(q - cursor); if (len >= sizeof el) len = sizeof el - 1;
        std::memcpy(el, cursor, len); el[len] = '\0';
        cursor = *q ? q + 1 : q;
        if (len == 0) continue;
        if (!normaliseElement(el, norm, sizeof norm)) continue;
        Command c;
        if (!parseLine(norm, c) || c.verb == Verb::Invalid || !isChainVerb(c.verb)) { if (badN) std::snprintf(bad, badN, "%s", el); return -1; }
        if (c.verb == Verb::Wait && c.hasA && c.a > 60) std::snprintf(norm, sizeof norm, ":PW%s60", c.b ? "R" : "");
        size_t l = std::strlen(norm);
        if (used + l + 1 >= n) { if (badN) std::snprintf(bad, badN, "too long"); return -1; }
        std::memcpy(out + used, norm, l); used += l; out[used] = '\0';
        ++count;
    }
    return count;
}

} // namespace uppity
