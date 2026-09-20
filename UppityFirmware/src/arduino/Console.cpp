#ifdef ARDUINO
#include <Arduino.h>
#include <esp_random.h>
#include "Console.h"
#include <Preferences.h>
#include "../core/EventFormat.h"
#include "../../Version.h"

namespace uppity {

void Console::begin(MotionSupervisor& sup, CommandQueue& q, EstopLatch& estop, I2cService& i2c, Print& out) {
    sup_ = &sup; q_ = &q; estop_ = &estop; i2c_ = &i2c; out_ = &out;
}

Ticks Console::travel() const { return sup_->current().lifterTravel > 0 ? sup_->current().lifterTravel : profileFor(cfg_.motor).defaultTravelTicks; }
int32_t Console::rnd(int32_t lo, int32_t hi) const { return lo + static_cast<int32_t>(esp_random() % static_cast<uint32_t>(hi - lo + 1)); }

bool Console::push(JobKind k, int32_t a, int32_t b, int32_t c) {
    MotionRequest r; r.kind = k; r.a = a; r.b = b; r.c = c; r.source = ReqSource::SerialUsb;
    uint32_t id = q_->push(r);
    if (id == 0) { out_->println("BUSY=queue_full"); return false; }
    out_->printf("QUEUED=%s id=%lu\n", jobName(static_cast<int>(k)), static_cast<unsigned long>(id));
    return true;
}

void Console::handleLine(const char* line, LineAssembler::Kind kind) {
    if (kind == LineAssembler::Kind::Ecosystem) {
        if (std::strncmp(line, "&ESTOP,RESET", 12) == 0) { if (estop_->release(millis())) out_->println("ESTOP=released"); }
        return;                                            // every other &-line is someone else's business
    }
    const char* cursor = line;
    char el[LineAssembler::kMax];
    bool first = true;
    while (nextChainElement(cursor, el, sizeof el)) {
        Command c;
        const char* text = el;
        char tmp[LineAssembler::kMax + 2];
        // chained elements may omit the ":P" after the first (legacy stored-sequence form): normalise
        if (!first && el[0] != ':' && el[0] != '#') { std::snprintf(tmp, sizeof tmp, ":P%.*s", LineAssembler::kMax - 3, el); text = tmp; }
        first = false;
        if (!parseLine(text, c)) continue;
        handleCommand(c);
    }
}

void Console::handleCommand(const Command& c) {
    const Snapshot& s = sup_->current();
    switch (c.verb) {
        case Verb::LiftTo: case Verb::LiftRandom: {
            int32_t pct = c.verb == Verb::LiftRandom ? rnd(0, 100) : clampv<int32_t>(c.a, 0, 100);
            int32_t speed = c.hasB ? (c.b == -2 ? rnd(30, 100) : c.b) * 10 : 0;
            push(JobKind::LifterSeek, pct * travel() / 100, speed, (pct >= 100 || pct <= 0) ? 1 : 0);
            break;
        }
        case Verb::SafeLower: push(JobKind::SafeLower, 0, c.hasA ? c.a * 10 : 0, 0); break;
        case Verb::RotAbs: case Verb::RotAbsRandom: case Verb::RotRel: case Verb::RotRelRandom: {
            bool rel = c.verb == Verb::RotRel || c.verb == Verb::RotRelRandom;
            bool rndv = c.verb == Verb::RotAbsRandom || c.verb == Verb::RotRelRandom;
            int32_t deg = rndv ? (rel ? rnd(-180, 180) : rnd(0, 359)) : c.a;
            int32_t spd = c.hasB ? (c.b == -2 ? rnd(40, 100) : c.b) * 10 : 0;
            int32_t mx = c.hasC ? (c.c == -2 ? rnd(40, 100) : c.c) * 10 : 0;
            if (!rel && deg == 0) push(JobKind::RotaryHome, 0, 0, 0);
            else push(rel ? JobKind::RotaryRelative : JobKind::RotaryAbsolute, deg, spd, mx);
            break;
        }
        case Verb::Spin: if (c.a == 0) push(JobKind::RotaryHome, 0, 0, 0); else push(JobKind::RotarySpin, clampv<int32_t>(c.a, -100, 100) * 10, 0, 0); break;
        case Verb::SpinRandom: { int32_t m = c.hasA ? clampv<int32_t>(c.a, 1, 100) : 80; int32_t v = rnd(-m, m); if (v == 0) v = m; push(JobKind::RotarySpin, v * 10, 0, 0); break; }
        case Verb::Estop: out_->println("ESTOP=latched"); break;
        case Verb::Resume: out_->println(estop_->release(millis()) ? "ESTOP=released" : "ESTOP=not_latched"); break;
        case Verb::State: printState(); break;
        case Verb::RotaryTest: push(JobKind::RotaryCharacterise, c.hasA ? clampv<int32_t>(c.a, 1, 10) : 3, 0, 0); break;
        case Verb::Calibrate: push(JobKind::LifterCalibrate, 0, 0, 0); break;
        case Verb::ClearFault: sup_->requestClearFault(); out_->println("FAULT=clear_requested"); break;
        case Verb::Config: printConfig(); break;
        case Verb::Motor:
            if (c.hasA && c.a >= 0 && c.a < static_cast<int32_t>(LifterMotor::Count)) { cfg_.motor = static_cast<LifterMotor>(c.a); cfg_.travelTicks = 0; cfg_.travelCalibrated = false; sup_->setConfig(cfg_); }
            out_->printf("MOTOR=%d %s\n", static_cast<int>(cfg_.motor), profileFor(cfg_.motor).name);
            break;
        case Verb::RotaryTpr:
            if (c.hasA && c.a >= 1000 && c.a <= 50000) { cfg_.rotary.ticksPerRev = c.a / 10.f; cfg_.rotary.confident = true; sup_->setConfig(cfg_); }
            out_->printf("ROTARYTPR=%.1f ticks/rev (RAM only; tenths, e.g. #PROTARYTPR11855)\n", cfg_.rotary.ticksPerRev);
            break;
        case Verb::Wiggle:
            if (c.hasA && c.a >= 0 && c.a <= 20) { cfg_.rotary.settleWiggleDeg = static_cast<uint8_t>(c.a); sup_->setConfig(cfg_); }
            out_->printf("WIGGLE=%u deg first swing (RAM only; 0 = no wiggle, the park's out-and-back still shows)\n", cfg_.rotary.settleWiggleDeg);
            break;
        case Verb::HomePark:
            if (c.hasA) { cfg_.rotary.parkOnLeadingEdge = c.a != 0; cfg_.rotary.parkApproach = c.a == 2 ? 1 : -1; sup_->setConfig(cfg_); }
            out_->printf("HOMEPARK=%d (0 = arc centre by count; 1 = first closure creeping in against the sweep, v3 style; 2 = creeping in with the sweep)\n", cfg_.rotary.parkOnLeadingEdge ? (cfg_.rotary.parkApproach > 0 ? 2 : 1) : 0);
            break;
        case Verb::HomeOffset:
            if (c.hasA && c.a >= -600 && c.a <= 600) { cfg_.rotary.homeOffsetTicks = static_cast<int16_t>(c.a); sup_->setConfig(cfg_); }
            out_->printf("HOMEOFFSET=%d ticks from the switch centre (RAM only; measure: :PA0, then :PD+-2 until straight, read rotary= in #PSTATE)\n", cfg_.rotary.homeOffsetTicks);
            break;
        case Verb::SafeSpin:
            if (c.hasA && c.a >= 0 && c.a <= 5000) { cfg_.safeSpinTicks = c.a; sup_->setConfig(cfg_); }
            out_->printf("SAFESPIN=%ld ticks (RAM only; 0 = only the top counts as clear)\n", (long)cfg_.safeSpinTicks);
            break;
        case Verb::V3Dump: printV3Prefs(); break;
        case Verb::Travel:
            if (c.hasA && c.a >= 100 && c.a <= 5000) { cfg_.travelTicks = c.a; cfg_.travelCalibrated = false; cfg_.safeSpinTicks = 0; sup_->setConfig(cfg_); }
            out_->printf("TRAVEL=%ld ticks (RAM only; 0 = profile default %u)\n", (long)cfg_.travelTicks, profileFor(cfg_.motor).defaultTravelTicks);
            break;
        case Verb::Invert:
            if (c.hasA) { cfg_.lifterInverted = c.a != 0; sup_->setConfig(cfg_); }
            out_->printf("INVERT=%d (RAM only%s)\n", cfg_.lifterInverted ? 1 : 0, c.hasA ? ", re-referenced from the limit switches" : "");
            break;
        case Verb::Unlock: cfg_.commissioned = true; sup_->setConfig(cfg_); out_->println("COMMISSIONED=1 (RAM only, phase 1 bench unlock)"); break;
        case Verb::Lock: cfg_.commissioned = false; sup_->setConfig(cfg_); out_->println("COMMISSIONED=0"); break;
        case Verb::Light: i2c_->setLightKit(static_cast<uint8_t>(clampv<int32_t>(c.a, 0, 7))); out_->printf("LIGHT=%d\n", i2c_->lightKit()); break;
        case Verb::LightRandom: { int m; do { m = rnd(0, 7); } while (m == 1); i2c_->setLightKit(static_cast<uint8_t>(m)); out_->printf("LIGHT=%d\n", m); break; }
        case Verb::Help:
            out_->println("CMDS=:PP<0-100>[,spd] :PPR :PH[spd] :PA<deg>[,spd][,max] :PAR :PD<deg>[,spd][,max] :PDR :PR<+-spd> :PRR[max] :PL<0-7> :PX");
            out_->println("CFG=#PRESUME #PSTATE #PROTARYTEST[n] #PSC #PCLEARFAULT #PCONFIG #PMOTOR[0-2] #PINVERT[0/1] #PTRAVEL<ticks> #PSAFESPIN<ticks> #PHOMEOFFSET<ticks> #PHOMEPARK<0/1/2> #PWIGGLE<deg> #PROTARYTPR<tenths> #PV3 #PUNLOCK #PLOCK #PHELP");
            break;
        case Verb::Wait: case Verb::PlaySeq: case Verb::Random: case Verb::RandomLevel: out_->println("UNSUPPORTED=phase2"); break;
        case Verb::Invalid: default: out_->println("INVALID"); break;
    }
    (void)s;
}

void Console::printState() {
    Snapshot s = sup_->current();
    out_->printf("STATE=%s lifter=%ld/%ld(%d%%) lifterConf=%s rotary=%ld ticks %ddeg homeConf=%s tpr=%.1f\n",
                 stateName(static_cast<int>(s.state)), (long)s.lifterTicks, (long)s.lifterTravel, s.lifterPct, lifterConfName(static_cast<int>(s.lifterConf)),
                 (long)s.rotaryTicks, s.rotaryDeg, homeConfName(static_cast<int>(s.homeConf)), s.ticksPerRev);
    out_->printf("LIMITS=top:%d bot:%d home:%d diag=[%d %d] enabled=%d faults=0x%lx estop=%d(%s) override=%lus\n",
                 s.topLimit, s.bottomLimit, s.homeSwitch, s.lifterDiag, s.rotaryDiag, s.enabled, (unsigned long)s.faults, s.estop,
                 EstopLatch::sourceName(static_cast<EstopLatch::Source>(s.estopSource)), (unsigned long)(s.overrideRemainingMs / 1000));
    out_->printf("JOB=%s last=%s %s %s tick=[max %luus late %lu n %lu]\n", jobName(static_cast<int>(s.activeJob)), jobName(static_cast<int>(s.lastJob)),
                 resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason), (unsigned long)s.cycleUsMax, (unsigned long)s.lateTicks, (unsigned long)s.tickCount);
}

// v3 kept its tuning in NVS namespace "uppityspinner"; the v4 flash layout leaves that
// partition where it was, so the builder's numbers are still readable. Read-only.
void Console::printV3Prefs() {
    Preferences p;
    if (!p.begin("uppityspinner", true)) { out_->println("V3=none (no uppityspinner namespace in NVS)"); return; }
    static const char* const keys[] = { "lftminpwr", "lftseekbotpwr", "rotminpwr", "lftdist", "minheight", "rotenccount", "driftpct", "aggression", "motortype", "wizstate", "expertmode" };
    out_->print("V3=");
    int n = 0;
    for (const char* k : keys) if (p.isKey(k)) { out_->printf(" %s=%ld", k, (long)p.getInt(k, -1)); ++n; }
    if (n == 0) out_->print(" (namespace present, no known keys)");
    out_->println();
    out_->println("V3 minheight is ticks in v3's frame (lftdist = its travel): #PSAFESPIN it, scaled if #PTRAVEL differs");
    p.end();
}

void Console::printConfig() {
    const MotionConfig& c = sup_->config();
    out_->printf("#PVERSION=%s\n#PMOTOR=%d (%s)\n#PEXPERT=%d\n#PINVERT=%d\n#PTRAVEL=%ld calibrated=%d\n#PMINUP=%d\n#PMINDOWN=%d\n#PSAFESPIN=%ld\n#PROTARYTPR=%.1f confident=%d arc=%u chatter=%u driveSign=%d\n#PHOMEOFFSET=%d\n#PROTARYDISABLED=%d\n#PCOMMISSIONED=%d\n#PDRIFT=%d\n",
                 UPPITY_FW_VERSION, static_cast<int>(c.motor), profileFor(c.motor).name, c.expertMode, c.lifterInverted, (long)c.travelTicks, c.travelCalibrated, c.minPowerUp, c.minPowerDown,
                 (long)c.safeSpinTicks, c.rotary.ticksPerRev, c.rotary.confident, c.rotary.contactArcTicks, c.rotary.chatterPerPass, c.rotary.driveSign, c.rotary.homeOffsetTicks, c.rotaryDisabled, c.commissioned, c.driftPct);
    if (c.travelCalibrated) {
        // slot = power in 5 % steps; vel = ticks/s at that power; over = coast ticks after the brake
        out_->print("#PCALUP  ");   for (int i = 0; i < kSlots; ++i) if (c.velUp[i] || c.overshootUp[i])     out_->printf(" %d%%:%u/%u", i * 5, c.velUp[i], c.overshootUp[i]);
        out_->print("\n#PCALDOWN");  for (int i = 0; i < kSlots; ++i) if (c.velDown[i] || c.overshootDown[i]) out_->printf(" %d%%:%u/%u", i * 5, c.velDown[i], c.overshootDown[i]);
        out_->println();
    }
}

} // namespace uppity
#endif
