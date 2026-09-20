#pragma once
#include <cstdint>
#include <cstring>
#include <cctype>
#include "Units.h"
// Marcduino grammar for the Uppity Spinner (":P..." motion, "#P..." configuration), parsed
// into Command structs. Pure. Phase 1 covers the motion verbs and the bench commands; Phase 2
// adds sequences, waits, lights, random mode and the full configuration set.
//   :PP<0-100>[,speed]  :PPR[,speed]  :PH[speed]  :PA<deg>[,spd][,max]  :PAR  :PD<deg>[,spd][,max]
//   :PDR  :PR<+-speed>  :PRR[max]  :PX
//   #PRESUME #PSTATE #PROTARYTEST[n] #PSC #PCLEARFAULT #PCONFIG #PMOTOR[n] #PUNLOCK #PLOCK #PHELP
namespace uppity {

enum class Verb : uint8_t {
    None = 0, Invalid,
    LiftTo, LiftRandom, SafeLower, RotAbs, RotAbsRandom, RotRel, RotRelRandom, Spin, SpinRandom, Estop,
    Resume, State, RotaryTest, Calibrate, ClearFault, Config, Motor, Invert, Travel, SafeSpin, V3Dump, HomeOffset, RotaryTpr, HomePark, Wiggle, Unlock, Lock, Help, Wait, Light, LightRandom, PlaySeq, Random, RandomLevel,
};

struct Command {
    Verb verb = Verb::None;
    int32_t a = 0, b = 0, c = 0;     // per verb; -1 = absent
    bool hasA = false, hasB = false, hasC = false;
    int32_t id = -1;                  // Marcduino ID prefix, -1 = none
    char letter = 0;                  // for RandomLevel: G/M/A
};

namespace parse {
inline bool readInt(const char*& p, int32_t& v) {
    const char* s = p; bool neg = false;
    if (*p == '-' || *p == '+') { neg = *p == '-'; ++p; }
    if (!isdigit(static_cast<unsigned char>(*p))) { p = s; return false; }
    int32_t n = 0; while (isdigit(static_cast<unsigned char>(*p))) { n = n * 10 + (*p - '0'); ++p; }
    v = neg ? -n : n; return true;
}
inline bool startsWith(const char* s, const char* pre) { return std::strncmp(s, pre, std::strlen(pre)) == 0; }
inline void readOptArgs(const char*& p, Command& c) {
    if (*p == ',') { ++p; if (*p == 'R') { ++p; c.b = -2; c.hasB = true; } else if (readInt(p, c.b)) c.hasB = true; }
    if (*p == ',') { ++p; if (*p == 'R') { ++p; c.c = -2; c.hasC = true; } else if (readInt(p, c.c)) c.hasC = true; }
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
                cmd.verb = Verb::Random; return true;
            default: cmd.verb = Verb::Invalid; return true;
        }
    }
    if (startsWith(p, "RESUME")) { cmd.verb = Verb::Resume; return true; }
    if (startsWith(p, "ROTARYTPR")) { p += 9; cmd.verb = Verb::RotaryTpr; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "ROTARYTEST")) { p += 10; cmd.verb = Verb::RotaryTest; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "STATE")) { cmd.verb = Verb::State; return true; }
    if (startsWith(p, "SC")) { cmd.verb = Verb::Calibrate; return true; }
    if (startsWith(p, "CLEARFAULT")) { cmd.verb = Verb::ClearFault; return true; }
    if (startsWith(p, "CONFIG")) { cmd.verb = Verb::Config; return true; }
    if (startsWith(p, "MOTOR")) { p += 5; cmd.verb = Verb::Motor; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "WIGGLE")) { p += 6; cmd.verb = Verb::Wiggle; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "HOMEPARK")) { p += 8; cmd.verb = Verb::HomePark; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "HOMEOFFSET")) { p += 10; cmd.verb = Verb::HomeOffset; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "SAFESPIN")) { p += 8; cmd.verb = Verb::SafeSpin; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "V3")) { cmd.verb = Verb::V3Dump; return true; }
    if (startsWith(p, "TRAVEL")) { p += 6; cmd.verb = Verb::Travel; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "INVERT")) { p += 6; cmd.verb = Verb::Invert; if (readInt(p, cmd.a)) cmd.hasA = true; return true; }
    if (startsWith(p, "UNLOCK")) { cmd.verb = Verb::Unlock; return true; }
    if (startsWith(p, "LOCK")) { cmd.verb = Verb::Lock; return true; }
    if (startsWith(p, "HELP")) { cmd.verb = Verb::Help; return true; }
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

} // namespace uppity
