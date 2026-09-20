#ifdef ARDUINO
#include <Arduino.h>
#include <esp_random.h>
#include "Console.h"
#include "../core/EventFormat.h"
#include "../../Version.h"
#include "../../Board.h"

namespace uppity {

uint32_t EspRandom::next() { return esp_random(); }

void Console::begin(const Deps& d) { d_ = d; }

void Console::line(const char* text) {
    d_.usb->println(text);
    if (s_.cmdTx && d_.cmd) d_.cmd->println(text);
}

// ---------------------------------------------------------------- boot / settings

void Console::boot() {
    bootMs_ = millis();
    bool had = d_.persist->begin(s_, cal_, *this);
    if (!had || !s_.v3Imported) importV3Now(false);
    sanitize(s_);
    applySettings(true);
    beginCmdSerial();
    const MotionConfig& m = d_.sup->config();
    (void)m;
    printf("CONFIG motor=%u(%s) inverted=%u travel=%ld%s safespin=%u tpr=%.1f homepark=%u wiggle=%u offset=%d commissioned=%u id=%u baud=%lu",
           s_.motor, profileFor(static_cast<LifterMotor>(s_.motor)).name, s_.inverted, static_cast<long>(effectiveTravel(s_, cal_)),
           s_.travelTicks ? " (#PTRAVEL)" : calibrationApplies(s_, cal_) ? " (calibrated)" : " (profile guess)", s_.safeSpinTicks, s_.ticksPerRev,
           s_.homePark, s_.wiggleDeg, s_.homeOffsetTicks, s_.commissioned, s_.marcId, static_cast<unsigned long>(s_.cmdBaud));
    if (!had) d_.persist->requestSaveSettings();          // first boot: write the blob so the next boot finds it
}

void Console::applySettings(bool boot) {
    d_.sup->setConfig(toMotionConfig(s_, cal_));
    if (boot) {
        d_.hal->setSwitchActiveLow(SwitchId::LifterTop, s_.lifterLimitsNC == 0);
        d_.hal->setSwitchActiveLow(SwitchId::LifterBottom, s_.lifterLimitsNC == 0);
        d_.hal->setSwitchActiveLow(SwitchId::RotaryHome, s_.rotaryHomeNC == 0);
    }
    d_.disp->setId(s_.marcId);
    d_.disp->setDedupMs(s_.dedupMs);
    d_.dir->setLightPolicy(s_.randomLight);
}

// Random mode off, and the light kit off with it when the firmware was the one that put it on.
void Console::stopRandom(const char* why) {
    if (d_.dir->stop(why) && s_.randomLight != 0) onLight(1);
}

void Console::beginCmdSerial() {
    if (!d_.cmd) return;
    d_.cmd->end();
    // GPIO0 is a boot strap; it is only driven as TX when the builder has wired it and asked for it.
    d_.cmd->begin(s_.cmdBaud, SERIAL_8N1, board::kCmdRx, s_.cmdTx ? board::kCmdTx : -1);
}

void Console::importV3Now(bool forced) {
    V3Prefs v;
    d_.persist->readV3(v);
    ImportReport rep;
    Settings before = s_;
    importV3(s_, v, rep);
    for (int i = 0; i < rep.n; ++i) line(rep.line[i]);
    if (rep.taken > 0 || forced) printf("V3 import: %d value%s taken%s", rep.taken, rep.taken == 1 ? "" : "s", forced ? " (forced)" : "");
    if (std::memcmp(&before, &s_, sizeof s_) != 0) { applySettings(false); d_.persist->requestSaveSettings(); }
}

RandomDirector::Limits Console::limits() const {
    RandomDirector::Limits l;
    const LifterMotorProfile& p = profileFor(static_cast<LifterMotor>(s_.motor));
    l.lifterMinPct = s_.minPowerUpPct ? s_.minPowerUpPct : (calibrationApplies(s_, cal_) ? static_cast<uint8_t>(cal_.minUp / 10) : p.defaultMinPowerPct);
    l.lifterMaxPct = static_cast<uint8_t>(effectiveMaxThrottle(static_cast<LifterMotor>(s_.motor), s_.expertMode != 0) / 10);
    l.rotaryMinPct = s_.rotaryMinPct;
    return l;
}

bool Console::randomActive() const { return d_.dir->active(); }

// ---------------------------------------------------------------- loop

void Console::update(Ms now) {
    // results the supervisor measured on its own: keep them
    if (d_.sup->takeCalibrationUpdated()) {
        takeCalibration(cal_, d_.sup->lastCalibration(), s_.motor, now);
        d_.persist->requestSaveCalibration();
        printf("CAL stored: travel=%u minUp=%u%% minDown=%u%% (profile %u)", cal_.travel, cal_.minUp / 10, cal_.minDown / 10, cal_.motor);
        if (s_.travelTicks) printf("CAL note: #PTRAVEL=%u still overrides the calibrated %u; send #PTRAVEL0 to use the calibrated figure", s_.travelTicks, cal_.travel);
        if (s_.minPowerUpPct || s_.minPowerDownPct) line("CAL note: #PMINUP/#PMINDOWN still override the calibrated minimum powers; set them to 0 to use the calibration");
        applySettings(false);
    }
    if (d_.sup->takeCharacterisationUpdated()) {
        if (takeCharacterisation(s_, d_.sup->config().rotary)) { d_.persist->requestSaveSettings(); printf("ROTARY stored: %.1f ticks/rev confident=%u arc=%u", s_.ticksPerRev, s_.tprConfident, s_.contactArcTicks); }
    }
    // trigger inputs: a debounced non-zero value plays that stored sequence
    if (d_.i2c->inputReads() != lastInputReads_) {
        lastInputReads_ = d_.i2c->inputReads();
        int v = triggers_.feed(d_.i2c->inputs());
        if (v) { char l[8]; std::snprintf(l, sizeof l, ":PS%d", v); printf("TRIGGER=%d", v); d_.disp->handleLine(l, ReqSource::Trigger, now); }
    }
    // random director
    Snapshot snap; if (!d_.pub->read(snap)) return;
    Command chain[Sequencer::kMaxElements];
    int n = d_.dir->next(now, !d_.seq->active(), snap, limits(), chain, Sequencer::kMaxElements);
    if (n > 0) d_.disp->submitDirector(chain, n);
    // flash
    d_.persist->update(snap, *d_.sup);
    if (restartPending_ && elapsed(now, restartAt_) > 300) { d_.usb->flush(); ESP.restart(); }
}

void Console::handleEcosystemLine(const char* l) {
    if (std::strncmp(l, "&ESTOP,RESET", 12) == 0) { if (d_.estop->release(millis())) line("ESTOP=released"); }
    // every other &-line is someone else's business (Sabé / Roam-A-Dome heartbeats)
}

void Console::noteEstop(bool latched) {
    if (latched) { stopRandom("ESTOP"); d_.seq->clear("ESTOP"); }
}

// ---------------------------------------------------------------- sequencer listener

void Console::onLight(int mode) {
    int m = mode;
    if (m < 0) { do { m = rng_.range(0, 7); } while (m == 1); }
    d_.i2c->setLightKit(static_cast<uint8_t>(m));
    printf("LIGHT=%d", m);
}
void Console::onRandom(const Command& c) {
    Ms now = millis();
    if (c.verb == Verb::RandomLevel) d_.dir->startLevel(c.letter == 'G' ? 0 : c.letter == 'M' ? 1 : 2, now);
    else if (c.hasA || c.hasB || c.hasC || c.hasD) d_.dir->startParams(c, limits(), now);
    else d_.dir->startLevel(s_.aggressiveness, now);
}
void Console::onRandomStop() { stopRandom("command"); }
void Console::onEstop() { bool was = d_.estop->tripped(); d_.estop->trip(EstopLatch::Source::Sequence, millis()); line(was ? "ESTOP=latched" : "ESTOP=latched (stored sequence)"); }

// ---------------------------------------------------------------- configuration commands

bool Console::refuseWhileMoving(const char* what) {
    if (d_.sup->activeJob() != JobKind::None || d_.seq->active()) { printf("%s=refused (something is moving; :PX or wait)", what); return true; }
    return false;
}

void Console::pushOperatorJob(JobKind k, int32_t a) {
    stopRandom("operator job"); d_.seq->clear("operator job");
    MotionRequest r; r.kind = k; r.a = a; r.source = ReqSource::SerialUsb;
    uint32_t id = d_.q->push(r);
    if (id == 0) line("BUSY=queue_full"); else printf("QUEUED=%s id=%lu", jobName(static_cast<int>(k)), static_cast<unsigned long>(id));
}

void Console::onConfig(const Command& c, const char* raw, ReqSource src) {
    (void)raw; (void)src;
    switch (c.verb) {
        case Verb::Resume: line(d_.estop->release(millis()) ? "ESTOP=released" : "ESTOP=not_latched"); break;
        case Verb::State: printState(); break;
        case Verb::Status: printStatus(); break;
        case Verb::Config: printConfig(); break;
        case Verb::Cal: printCal(); break;
        case Verb::V3Dump: printV3(); break;
        case Verb::V3Import: importV3Now(true); break;
        case Verb::Help: printHelp(); break;
        case Verb::ClearFault: d_.sup->requestClearFault(); line("FAULT=clear_requested"); break;
        case Verb::RotaryTest: pushOperatorJob(JobKind::RotaryCharacterise, c.hasA ? clampv<int32_t>(c.a, 1, 10) : 3); break;
        case Verb::Calibrate: pushOperatorJob(JobKind::LifterCalibrate, 0); break;
        case Verb::Unlock: s_.commissioned = 1; applySettings(false); d_.persist->requestSaveSettings(); line("COMMISSIONED=1 (saved: the board boots unlocked from now on; #PLOCK to undo)"); break;
        case Verb::Lock: s_.commissioned = 0; applySettings(false); d_.persist->requestSaveSettings(); line("COMMISSIONED=0 (saved)"); break;
        case Verb::Set: setKey(c); break;
        case Verb::RotaryToggle:
            s_.rotaryDisabled = s_.rotaryDisabled ? 0 : 1; applySettings(false); d_.persist->requestSaveSettings();
            printf("#PROTARYOFF=%u (%s, saved)", s_.rotaryDisabled, s_.rotaryDisabled ? "rotary disabled: no homing, descents never wait for it" : "rotary enabled");
            break;
        case Verb::LimitPolarity: {
            uint8_t nc = c.letter == 'C' ? 1 : 0;
            if (c.a == 'L') s_.lifterLimitsNC = nc; else s_.rotaryHomeNC = nc;
            d_.persist->requestSaveSettings();
            printf("#P%s=%u (%s; takes effect after #PRESTART)", c.a == 'L' ? "LIFTERNC" : "ROTARYNC", nc, nc ? "normally closed" : "normally open");
            break;
        }
        case Verb::StoreSeq: {
            int n = clampv<int32_t>(c.a, 0, SequenceStore::kCount - 1);
            char canon[SequenceStore::kMaxLen + 1], bad[64];
            int els = canonicalSequence(c.text ? c.text : "", canon, sizeof canon, bad, sizeof bad);
            if (els < 0) { printf("INVALID=sequence element '%s' (nothing stored)", bad); break; }
            if (els == 0) { printf("INVALID=empty sequence %d (use #PD%d to delete)", n, n); break; }
            printf("Sequence [%d]: %s", n, canon);
            if (!d_.persist->storeSequence(n, canon)) line("BUSY=too many stores pending; wait for PERSIST=saved");
            break;
        }
        case Verb::ListSeq: printSequences(); break;
        case Verb::DelSeq: { int n = clampv<int32_t>(c.a, 0, SequenceStore::kCount - 1); if (!d_.persist->eraseSequence(n)) line("BUSY=too many stores pending"); break; }
        case Verb::SeqName: {
            int n = clampv<int32_t>(c.a, 0, SequenceStore::kCount - 1);
            char name[SequenceStore::kNameLen + 1]; std::snprintf(name, sizeof name, "%s", c.text ? c.text : "");
            if (!d_.persist->nameSequence(n, name)) line("BUSY=too many stores pending");
            break;
        }
        case Verb::Zero: if (!d_.persist->clearSequences()) line("BUSY"); else line("SEQ clearing all stored sequences"); break;
        case Verb::Factory:
            if (refuseWhileMoving("FACTORY")) break;
            d_.persist->factoryReset(); line("FACTORY=done, restarting"); restartPending_ = true; restartAt_ = millis();
            break;
        case Verb::Restart:
            if (refuseWhileMoving("RESTART")) break;
            if (d_.persist->pending()) line("RESTART=waiting for PERSIST=saved first");
            line("RESTART=in 300 ms"); restartPending_ = true; restartAt_ = millis();
            break;
        case Verb::Invalid: default: line("INVALID"); break;
    }
}

void Console::setKey(const Command& c) {
    const SettingKey* k = findSettingKey(c.key, std::strlen(c.key));
    char buf[64];
    if (!k) { printf("INVALID=#P%s (unknown; #PHELP lists the keys)", c.key); return; }
    if (!c.hasA) { formatSetting(s_, *k, buf, sizeof buf); printf("%s  (%s)", buf, k->help); return; }
    Settings before = s_;
    if (!k->set(s_, c.a)) { printf("OUT_OF_RANGE=#P%s%ld (%s)", k->name, static_cast<long>(c.a), k->help); return; }
    sanitize(s_);
    bool changed = std::memcmp(&before, &s_, sizeof s_) != 0;
    const char* note = "";
    if (std::strcmp(k->name, "MOTOR") == 0 && before.motor != s_.motor) {
        // a different lifter is a different mechanism: everything measured on the old one goes
        s_.commissioned = 0; s_.travelTicks = 0; s_.safeSpinTicks = 0; s_.minPowerUpPct = s_.minPowerDownPct = s_.approachPowerPct = 0;
        note = " (new profile: LOCKED again, travel/safe-spin/min powers cleared, calibration ignored until #PSC)";
    } else if (std::strcmp(k->name, "INVERT") == 0) note = " (re-referenced from the limit switches)";
    else if (std::strcmp(k->name, "TRAVEL") == 0 && s_.safeSpinTicks) note = " (SAFESPIN kept: it is ticks above the bottom switch, not a share of travel)";
    else if (std::strcmp(k->name, "ROTARYTPR") == 0) note = s_.ticksPerRev > 0 ? " (marked confident: no two-turn measurement on the first rotary move)" : " (cleared: the next rotary move measures it)";
    else if (std::strcmp(k->name, "LIFTERNC") == 0 || std::strcmp(k->name, "ROTARYNC") == 0) note = " (takes effect after #PRESTART)";
    else if (std::strcmp(k->name, "BAUD") == 0 || std::strcmp(k->name, "CMDTX") == 0) { beginCmdSerial(); note = " (command UART reopened)"; }
    else if (std::strcmp(k->name, "COMMISSIONED") == 0) note = s_.commissioned ? " (unlocked)" : " (LOCKED)";
    applySettings(false);
    if (changed) d_.persist->requestSaveSettings();
    formatSetting(s_, *k, buf, sizeof buf);
    printf("%s%s%s", buf, changed ? " (saved)" : " (unchanged)", note);
}

// ---------------------------------------------------------------- reports

void Console::printState() {
    Snapshot s; if (!d_.pub->read(s)) return;
    printf("STATE=%s lifter=%ld/%ld(%d%%) lifterConf=%s rotary=%ld ticks %ddeg homeConf=%s tpr=%.1f",
           stateName(static_cast<int>(s.state)), (long)s.lifterTicks, (long)s.lifterTravel, s.lifterPct, lifterConfName(static_cast<int>(s.lifterConf)),
           (long)s.rotaryTicks, s.rotaryDeg, homeConfName(static_cast<int>(s.homeConf)), s.ticksPerRev);
    printf("LIMITS=top:%d bot:%d home:%d diag=[%d %d] enabled=%d faults=0x%lx estop=%d(%s) override=%lus",
           s.topLimit, s.bottomLimit, s.homeSwitch, s.lifterDiag, s.rotaryDiag, s.enabled, (unsigned long)s.faults, s.estop,
           EstopLatch::sourceName(static_cast<EstopLatch::Source>(s.estopSource)), (unsigned long)(s.overrideRemainingMs / 1000));
    printf("JOB=%s id=%lu last=%s %s %s (id %lu) tick=[max %luus late %lu n %lu]", jobName(static_cast<int>(s.activeJob)), (unsigned long)s.activeJobId, jobName(static_cast<int>(s.lastJob)),
           resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason), (unsigned long)s.lastProgramId, (unsigned long)s.cycleUsMax, (unsigned long)s.lateTicks, (unsigned long)s.tickCount);
    printf("SEQ=%s random=%s light=%u inputs=%u persist=%s", d_.seq->active() ? "running" : "idle", d_.dir->active() ? "on" : "off", d_.i2c->lightKit(), d_.i2c->inputs(), d_.persist->pending() ? "pending" : "saved");
}

void Console::printConfig() {
    printf("CONFIG=begin v%s schema=%u (every #P line below can be pasted back)", UPPITY_FW_VERSION, kSettingsSchema);
    char buf[64];
    for (int i = 0; i < kSettingKeyCount; ++i) { formatSetting(s_, kSettingKeys[i], buf, sizeof buf); line(buf); }
    printSequences();
    printf("CONFIG=end travel_in_use=%ld%s", static_cast<long>(effectiveTravel(s_, cal_)), s_.travelTicks ? " (#PTRAVEL)" : calibrationApplies(s_, cal_) ? " (calibrated)" : " (profile guess)");
}

void Console::printCal() {
    if (!cal_.valid) { line("CAL=none (run #PSC; needs the rotary home and the head clear)"); return; }
    printf("CAL=%s travel=%u minUp=%u%% minDown=%u%% profile=%u measuredAt=%lus", calibrationApplies(s_, cal_) ? "in use" : "ignored (other profile)", cal_.travel, cal_.minUp / 10, cal_.minDown / 10, cal_.motor, static_cast<unsigned long>(cal_.measuredAtMs / 1000));
    // slot = power in 5 % steps; vel = ticks/s at that power; over = coast ticks after the brake
    String up = "CALUP  ", down = "CALDOWN";
    for (int i = 0; i < kSlots; ++i) { if (cal_.slotValidUp[i]) { up += ' '; up += i * 5; up += "%:"; up += cal_.velUp[i]; up += '/'; up += cal_.overUp[i]; } if (cal_.slotValidDown[i]) { down += ' '; down += i * 5; down += "%:"; down += cal_.velDown[i]; down += '/'; down += cal_.overDown[i]; } }
    line(up.c_str()); line(down.c_str());
}

void Console::printSequences() {
    char text[SequenceStore::kMaxLen + 1], name[SequenceStore::kNameLen + 1];
    int n = 0;
    for (int i = 0; i < SequenceStore::kCount; ++i) {
        if (!d_.persist->sequences().read(i, text, sizeof text)) continue;
        printf("#PS%d:%s", i, text); ++n;
        if (d_.persist->sequences().readName(i, name, sizeof name)) printf("#PSEQNAME%d=%s", i, name);
    }
    if (n == 0) line("SEQ=none stored");
}

void Console::printStatus() {
    Ms up = elapsed(millis(), bootMs_) / 1000;
    printf("VERSION=%s phase2 built %s %s", UPPITY_FW_VERSION, __DATE__, __TIME__);
    printf("UPTIME=%lus commissioned=%u random=%s seq=%s persist=%s saves=%lu", static_cast<unsigned long>(up), s_.commissioned, d_.dir->active() ? "on" : "off", d_.seq->active() ? "running" : "idle", d_.persist->pending() ? "pending" : "saved", static_cast<unsigned long>(d_.persist->saves()));
    printf("SERIAL=usb+cmd@%lu id=%u dedup=%ums droppedForId=%lu duplicates=%lu cmdTx=%u", static_cast<unsigned long>(s_.cmdBaud), s_.marcId, s_.dedupMs, static_cast<unsigned long>(d_.disp->droppedForId()), static_cast<unsigned long>(d_.disp->droppedDuplicates()), s_.cmdTx);
    printf("EXPANDER=%s errors=%lu inputs=%u light=%u", d_.i2c->ok() ? "ok" : "FAILING", static_cast<unsigned long>(d_.i2c->errors()), d_.i2c->inputs(), d_.i2c->lightKit());
    line("WIFI=none (phase 3)");
}

void Console::printV3() {
    V3Prefs v;
    if (!d_.persist->readV3(v)) { line("V3=none (no uppityspinner namespace in NVS)"); return; }
    if (!v.present) { line("V3=namespace present, no known keys"); return; }
    printf("V3= lftminpwr=%ld lftseekbotpwr=%ld rotminpwr=%ld lftdist=%ld minheight=%ld rotenccount=%ld driftpct=%ld aggression=%ld motortype=%ld expertmode=%ld wizstate=%ld",
           (long)v.lftminpwr, (long)v.lftseekbotpwr, (long)v.rotminpwr, (long)v.lftdist, (long)v.minheight, (long)v.rotenccount, (long)v.driftpct, (long)v.aggression, (long)v.motortype, (long)v.expertmode, (long)v.wizstate);
    printf("V3 import %s; #PV3IMPORT runs it again (conservative: defaults are never taken)", s_.v3Imported ? "already ran" : "has not run");
}

void Console::printHelp() {
    line("CMDS=:PP<0-100>[,spd] :PPR :PH[spd] :PA<deg>[,spd][,max] :PAR :PD<deg>[,spd][,max] :PDR :PR<+-spd> :PRR[max] :PL<0-7> :PLR :PW[R]<s> :PS<n> :PM :PMG/M/A :PM,lift,rot,min,max :PX  (chain with ':')");
    line("CFG=#PRESUME #PSTATE #PSTATUS #PCONFIG #PCAL #PSC #PROTARYTEST[n] #PCLEARFAULT #PUNLOCK #PLOCK #PS<n>:<seq> #PL #PD<n> #PSEQNAME<n>=<name> #PZERO #PR #PNCL/#PNOL/#PNCR/#PNOR #PV3 #PV3IMPORT #PRESTART #PFACTORY #PHELP");
    String keys = "KEYS=";
    for (int i = 0; i < kSettingKeyCount; ++i) { keys += "#P"; keys += kSettingKeys[i].name; keys += ' '; }
    keys += " (#PKEY<n> or #PKEY=<n> sets and saves, #PKEY reads)";
    line(keys.c_str());
}

} // namespace uppity
#endif
