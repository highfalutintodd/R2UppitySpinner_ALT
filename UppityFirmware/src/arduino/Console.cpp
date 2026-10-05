#ifdef ARDUINO
#include <Arduino.h>
#include <esp_random.h>
#include <esp_app_desc.h>
#include "Console.h"
#include "../core/MeshProtocol.h"
#include "../core/EventFormat.h"
#include "../../Version.h"
#include "../../Board.h"

namespace uppity {

uint32_t EspRandom::next() { return esp_random(); }

// Eight hex digits of the ELF hash: unique per build, unlike __DATE__/__TIME__, which come from
// whichever source file happened to recompile (bench 2026-09-27: #PSTATUS showed a five-hour-old
// stamp on a fresh build because Console.cpp was served from the build cache).
const char* Console::buildId() {
    static char id[9] = {};
    if (!id[0]) { const esp_app_desc_t* d = esp_app_get_description(); std::snprintf(id, sizeof id, "%02x%02x%02x%02x", d->app_elf_sha256[0], d->app_elf_sha256[1], d->app_elf_sha256[2], d->app_elf_sha256[3]); }
    return id;
}

// The commissioning flow's view of the console: settings, the job queue, the sequencer and the
// random director, all touched from loop() only (the flow runs from Console::update and onConfig).
struct Console::FlowHost final : CommissionFlow::Host {
    Console& c;
    explicit FlowHost(Console& con) : c(con) {}
    Settings& settings() override { return c.s_; }
    const Calibration& calibration() override { return c.cal_; }
    const CharacteriseResult& characterisation() override { return c.d_.sup->lastCharacterisation(); }
    void settingsChanged() override { c.applySettings(false); c.d_.persist->requestSaveSettings(); }
    uint32_t pushJob(const MotionRequest& r) override { c.stopRandom("commissioning"); c.d_.seq->clear("commissioning"); return c.d_.q->push(r); }
    bool runChain(const Command* els, int n) override { return c.d_.seq->submit(els, n, ReqSource::Commission, false); }
    bool chainActive() override { return c.d_.seq->active(); }
    void cancelMotion() override { c.stopRandom("commissioning stopped"); c.d_.seq->clear("commissioning stopped"); c.d_.q->cancelAll(); }
    void randomStart(int level) override { c.d_.dir->startLevel(level, millis()); }
    void randomStop() override { c.stopRandom("commissioning"); }
    uint32_t randomActions() override { return c.d_.dir->actions(); }
    bool expanderOk() override { return c.d_.i2c->ok(); }
};

void Console::begin(const Deps& d) {
    d_ = d;
    static FlowHost host(*this);
    host_ = &host;
    flow_.begin(host, *this);
}

void Console::line(const char* text) {
    d_.usb->println(text);
    if (s_.cmdTx && d_.cmd) d_.cmd->println(text);
    if (d_.log) d_.log->line(text);              // the web page's log tail reads from here
}

void Console::handleWebLine(const char* l) {
    if (std::strcmp(l, "!STOP") == 0) { stopAll("web"); return; }   // the page's soft stop
    d_.disp->handleLine(l, ReqSource::Web, millis());
}

// End the show, drop the chain, cancel the running job and anything queued behind it (D2).
void Console::stopAll(const char* why) {
    char w[40]; std::snprintf(w, sizeof w, "%s stop", why);
    stopRandom(w); d_.seq->clear(w); d_.q->cancelAll(); printf("STOP=%s", why);
}

void Console::writeStateJson(JsonWriter& j) {
    j.key("mesh");
    if (mesh_) mesh_->writeJson(j);
    else { j.beginObject(); j.kv("enabled", s_.meshEnabled != 0); j.kv("on", false); j.kv("missing", meshMissing(s_) ? meshMissing(s_) : ""); j.endObject(); }
    j.key("random"); j.beginObject(); j.kv("on", d_.dir->active()); j.kv("level", static_cast<int>(s_.aggressiveness)); j.endObject();
    j.kv("seq", d_.seq->active());
    j.kv("light", static_cast<int>(d_.i2c->lightKit()));
    j.kv("safeSpin", static_cast<long>(s_.safeSpinTicks));
    j.kv("travelSet", static_cast<long>(effectiveTravel(s_, cal_)));
    j.kv("persist", d_.persist->pending() ? "pending" : "saved");
    j.kv("theme", static_cast<int>(s_.theme));
    j.kv("build", buildId());
    j.kv("inputs", static_cast<int>(d_.i2c->inputs()));
    j.kv("expander", d_.i2c->ok());
    j.key("commission"); j.beginObject();
    j.kv("step", static_cast<int>(flow_.step())); j.kv("name", CommissionFlow::stepName(flow_.step())); j.kv("active", flow_.active());
    j.kv("run", CommissionFlow::runName(flow_.run())); j.kv("msg", flow_.message()); j.kv("detail", static_cast<long>(flow_.detail()));
    j.kv("prevSafeSpin", static_cast<long>(flow_.previousSafeSpin()));
    j.endObject();
}

void Console::writePowersJson(JsonWriter& j) {
    PowerSet p = powerSet(s_, cal_);
    j.beginObject();
    j.kv("floorUp", static_cast<int>(p.floorUp / 10)); j.kv("floorDown", static_cast<int>(p.floorDown / 10));
    j.kv("slowUp", static_cast<int>(p.slowUp / 10)); j.kv("slowDown", static_cast<int>(p.slowDown / 10)); j.kv("cap", static_cast<int>(p.cap / 10));
    j.kv("rotaryMin", static_cast<int>(p.rotaryMinPct)); j.kv("rotarySlow", static_cast<int>(p.rotarySlowPct));
    j.kv("profile", profileFor(static_cast<LifterMotor>(s_.motor)).name);
    j.kv("travel", static_cast<long>(effectiveTravel(s_, cal_)));
    j.endObject();
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
    MotionConfig m = toMotionConfig(s_, cal_);
    m.commissioningActive = flow_.active();          // the flow lifts the lock for its own steps while it is active
    d_.sup->setConfig(m);
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
    // A TX buffer, so replies echoed at 9600 baud (#PCMDTX1) never hold up loop() (~1 ms a byte).
    d_.cmd->setTxBufferSize(512);
    // GPIO0 is a boot strap; it is only driven as TX when the builder has wired it and asked for it.
    d_.cmd->begin(s_.cmdBaud, SERIAL_8N1, board::kCmdRx, s_.cmdTx ? board::kCmdTx : -1);
    if (d_.cmdFront) d_.cmdFront->rehook();       // end() dropped the receive hooks (code review 2026-10: the bus went deaf)
}

void Console::importV3Now(bool forced) {
    V3Prefs v;
    d_.persist->readV3(v);
    ImportReport rep;
    Settings before = s_;
    // forced (#PV3IMPORT): a v3 value only fills a setting this board has no value of its own for
    importV3(s_, v, rep, forced, forced && calibrationApplies(s_, cal_));
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
        printf("CAL stored: travel=%u minUp=%u%% minDown=%u%% rest=%u (profile %u)", cal_.travel, cal_.minUp / 10, cal_.minDown / 10, cal_.bottomRest, cal_.motor);
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
    Snapshot snap; if (!d_.pub->read(snap)) return;
    // commissioning: watches its own programs and queues the next stage
    flow_.update(snap, now);
    // random director
    Command chain[Sequencer::kMaxElements];
    int n = d_.dir->next(now, !d_.seq->active(), snap, limits(), chain, Sequencer::kMaxElements);
    if (n > 0) d_.disp->submitDirector(chain, n);
    // flash
    d_.persist->update(snap, *d_.sup);
    if (restartKind_ && elapsed(now, restartAt_) > 300) {
        bool idle = snap.activeJob == JobKind::None && !snap.enabled && !d_.persist->pending();
        bool late = elapsed(now, restartAt_) > 5000;
        if (idle || late) {
            if (!idle) line("RESTART=after 5 s without the board going idle; a pending save may be lost");
            if (restartKind_ == 2) d_.persist->factoryReset();     // disarmed here, like every other NVS write
            d_.usb->flush(); ESP.restart();
        }
    }
}

void Console::handleEcosystemLine(const char* l) {
    if (std::strncmp(l, "&ESTOP,RESET", 12) == 0) { if (d_.estop->release(millis())) line("ESTOP=released"); }
    // every other &-line is someone else's business (Sabé / Roam-A-Dome heartbeats)
}

// Restarts wait for the board to be idle (Console::update): random mode and any chain end now, and
// the intake is held so nothing new starts in the meantime (code review 2026-10: a restart 300 ms
// after the request dropped a save that was deferred while the drivers were on, and could cut off
// a random action that started in between).
void Console::requestRestart(uint8_t kind, const char* why) {
    stopRandom(why); d_.seq->clear(why);
    d_.sup->setHoldIntake(MotionSupervisor::kHoldRestart, true);
    restartKind_ = kind; restartAt_ = millis();
}

void Console::noteEstop(bool latched) {
    if (latched) { stopRandom("ESTOP"); d_.seq->clear("ESTOP"); }
}

void Console::onEvent(const Event& e) {
    flow_.onEvent(e);
    if (e.code == Ev::JobEnd && e.c == static_cast<int32_t>(Reason::NoEdge) && e.a == static_cast<int32_t>(JobKind::RotaryCharacterise))
        line("HINT=the home switch never closed: check its wiring; a normally-closed switch needs #PROTARYNC1 then #PRESTART");
    if (e.code == Ev::JobEnd && e.c == static_cast<int32_t>(Reason::WrongLimit))
        printf("HINT=the mast moved the other way (the switch it was leaving closed again): #PINVERT%d, then try again", s_.inverted ? 0 : 1);
}

// Rescue (docs/REWRITE-PLAN.md §4): the 60 s override, hold-to-jog, the lifter nudge, "set home here".
void Console::onRescue(const Command& c, ReqSource src) {
    Ms now = millis();
    switch (c.verb) {
        case Verb::Override:
            if (c.a == 0) { d_.q->setOverride(0); line("OVERRIDE=off"); return; }
            // an attended, local action: never from the Marcduino bus (other boards) or the mesh
            if (src != ReqSource::SerialUsb && src != ReqSource::Web) { line("OVERRIDE=refused (only from the USB console or the web page)"); return; }
            if (d_.estop->tripped()) { line("OVERRIDE=refused (E-STOP latched: reset it first)"); return; }
            { Ms until = now + CommandQueue::kOverrideMs; d_.q->setOverride(until ? until : 1); }   // 0 means off
            line("OVERRIDE=on (60 s; #POVERRIDE0 ends it)");
            return;
        case Verb::Jog: {
            if (c.letter == 0) { d_.q->jogRelease(now); return; }
            stopRandom("jog"); d_.seq->clear("jog");
            // A typed #PJOG runs for one lease (2 s). The page's lease comes from /api/jog alone: renewing
            // it here too would restart a jog whose button was already released before this line ran.
            if (src != ReqSource::Web) d_.q->jogKeepalive(now);
            PowerSet pw = powerSet(s_, cal_);
            MotionRequest r; r.source = src;
            int pct;
            if (c.letter == 'L') { pct = clampv<int>(c.a ? c.a : (c.b > 0 ? pw.slowUp : pw.slowDown) / 10, 12, pw.cap / 10); r.kind = JobKind::LifterCreep; r.a = c.b; r.b = pct * 10; r.c = 1; }
            else { pct = clampv<int>(c.a ? c.a : pw.rotarySlowPct, 12, 100); r.kind = JobKind::RotaryJog; r.a = c.b * pct * 10; }
            uint32_t id = d_.q->push(r);
            if (id == 0) { line("BUSY=queue_full"); return; }
            printf("JOG=%s %s at %d%% id=%lu (held while the page renews it; stops 2 s after the last renewal)", c.letter == 'L' ? "lifter" : "rotary",
                   c.letter == 'L' ? (c.b > 0 ? "up" : "down") : (c.b > 0 ? "+" : "-"), pct, static_cast<unsigned long>(id));
            return;
        }
        case Verb::Nudge: {
            stopRandom("nudge"); d_.seq->clear("nudge");
            int32_t t = clampv<int32_t>(c.a, -MotionSupervisor::kMaxStepTicks, MotionSupervisor::kMaxStepTicks);
            PowerSet pw = powerSet(s_, cal_);
            MotionRequest r; r.kind = JobKind::LifterCreep; r.a = t; r.c = 2; r.source = src;
            uint32_t id = d_.q->push(r);
            if (id == 0) { line("BUSY=queue_full"); return; }
            printf("NUDGE=%+ld ticks at %d%% id=%lu (continuous; stops on a switch, a stall, the policy floor, or after %ld ticks)", static_cast<long>(t),
                   (t > 0 ? pw.slowUp : pw.slowDown) / 10, static_cast<unsigned long>(id), static_cast<long>(t < 0 ? -t : t));
            return;
        }
        case Verb::SetHome:
            // the operator vouches that the head is lined up: someone has to be looking at it
            if (src != ReqSource::SerialUsb && src != ReqSource::Web) { line("SETHOME=refused (only from the USB console or the web page)"); return; }
            if (d_.estop->tripped()) { line("SETHOME=refused (E-STOP latched)"); return; }
            if (s_.rotaryDisabled) { line("SETHOME=refused (the rotary is disabled)"); return; }
            if (refuseWhileMoving("SETHOME")) return;
            d_.sup->requestSetRotaryHome();
            line("SETHOME=requested (the head's position now becomes home)");
            return;
        default: return;
    }
}

void Console::onCommission(const Command& c) {
    Snapshot snap; if (!d_.pub->read(snap)) { line("BUSY"); return; }
    switch (c.verb) {
        case Verb::SafeSpinHere: flow_.safeSpinHere(snap); return;
        case Verb::Commission:
            switch (c.letter) {
                case 'S': flow_.start(snap); return;
                case 'P': flow_.stop("operator"); return;
                case 'R': flow_.runStep(snap, millis()); return;
                case 'N': flow_.next(snap); return;
                case 'X': flow_.reset(); return;
                case 'B': flow_.bypass(); return;
                default:
                    printf("COMMISSION=step %d/%d (%s) active=%d run=%s commissioned=%u%s%s", flow_.step(), CommissionFlow::kSteps, CommissionFlow::stepName(flow_.step()),
                           flow_.active(), CommissionFlow::runName(flow_.run()), s_.commissioned, flow_.message()[0] ? " msg=" : "", flow_.message());
                    return;
            }
        default: return;
    }
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
void Console::onRandomStalled() { stopRandom("stalled"); }
void Console::onEstop() { bool was = d_.estop->tripped(); d_.estop->trip(EstopLatch::Source::Sequence, millis()); line(was ? "ESTOP=latched" : "ESTOP=latched (stored sequence)"); }

// ---------------------------------------------------------------- configuration commands

bool Console::refuseWhileMoving(const char* what) {
    if (d_.sup->activeJob() != JobKind::None || d_.seq->active()) { printf("%s=refused (something is moving; :PX or wait)", what); return true; }
    return false;
}

void Console::pushOperatorJob(JobKind k, int32_t a, int32_t c, ReqSource src) {
    stopRandom("operator job"); d_.seq->clear("operator job");
    MotionRequest r; r.kind = k; r.a = a; r.c = c; r.source = src;   // the line's own source: the override is for attended ones
    uint32_t id = d_.q->push(r);
    if (id == 0) line("BUSY=queue_full"); else printf("QUEUED=%s id=%lu", jobName(static_cast<int>(k)), static_cast<unsigned long>(id));
}

void Console::onConfig(const Command& c, const char* raw, ReqSource src) {
    (void)raw;                                    // never printed: it may carry a password
    // The mesh is unattended and over the air: motion, questions and #PRESUME only (MeshProtocol.h).
    if (src == ReqSource::Mesh && !meshMayRunConfig(c.verb)) {
        line("MESH=refused a #P command from the mesh: settings, commissioning, unlocking and restarts come from the USB console or the page");
        return;
    }
    switch (c.verb) {
        case Verb::Override: case Verb::Jog: case Verb::Nudge: case Verb::SetHome: onRescue(c, src); break;
        case Verb::Commission: case Verb::SafeSpinHere: onCommission(c); break;
        case Verb::Resume: line(d_.estop->release(millis()) ? "ESTOP=released" : "ESTOP=not_latched"); break;
        case Verb::State: printState(); break;
        case Verb::Status: printStatus(); break;
        case Verb::Config: printConfig(); break;
        case Verb::Cal: printCal(); break;
        case Verb::V3Dump: printV3(); break;
        case Verb::V3Import: if (!refuseWhileMoving("V3IMPORT")) importV3Now(true); break;
        case Verb::Help: printHelp(); break;
        case Verb::ClearFault: d_.sup->requestClearFault(); line("FAULT=clear_requested"); break;
        case Verb::RotaryTest: pushOperatorJob(JobKind::RotaryCharacterise, c.hasA ? clampv<int32_t>(c.a, 1, 10) : 3, 8, src); break;   // 8: home afterwards
        case Verb::Calibrate: pushOperatorJob(JobKind::LifterCalibrate, 0, 0, src); break;
        case Verb::Unlock: s_.commissioned = 1; applySettings(false); d_.persist->requestSaveSettings(); line("COMMISSIONED=1 (saved: the board boots unlocked from now on; #PLOCK to undo)"); break;
        case Verb::Lock: s_.commissioned = 0; applySettings(false); d_.persist->requestSaveSettings(); line("COMMISSIONED=0 (saved)"); break;
        case Verb::Set: setKey(c); break;
        case Verb::RotaryToggle:
            s_.rotaryDisabled = s_.rotaryDisabled ? 0 : 1; applySettings(false); d_.persist->requestSaveSettings();
            printf("#PROTARYOFF=%u (%s, saved)", s_.rotaryDisabled, s_.rotaryDisabled ? "rotary disabled: no homing, descents never wait for it" : "rotary enabled");
            if (s_.rotaryDisabled && d_.sup->headMayBeTurned())
                line("ROTARYOFF note: the head may be turned right now, so descents still stop at the safe-spin height until #PRESTART (or turn the rotary back on and home it)");
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
            line("FACTORY=clearing settings, calibration and sequences, then restarting (once the motors are off)"); requestRestart(2, "factory reset");
            break;
        case Verb::WifiReset:
            if (refuseWhileMoving("WIFIRESET")) break;
            s_.staSsid[0] = '\0'; s_.staPass[0] = '\0'; std::snprintf(s_.apPass, sizeof s_.apPass, "Astromech"); s_.wifiEnabled = 1; s_.apChannel = 1;
            d_.persist->requestSaveSettings();
            line("WIFI=reset to the access point only, password \"Astromech\"; restarting once it is saved"); requestRestart(1, "wifi reset");
            break;
        case Verb::Restart:
            if (refuseWhileMoving("RESTART")) break;
            line(d_.persist->pending() ? "RESTART=waiting for PERSIST=saved first (the drivers switch off 2 s after a move)" : "RESTART=in 300 ms");
            requestRestart(1, "restart");
            break;
        case Verb::Invalid: default: line("INVALID"); break;
    }
}

void Console::setKey(const Command& c) {
    if (const StringKey* sk = findStringKey(c.key, std::strlen(c.key))) {
        if (!c.text) { if (sk->secret) printf("#P%s=%s  (%s)", sk->name, stringField(s_, *sk)[0] ? "(set)" : "(empty)", sk->help); else printf("#P%s=%s  (%s)", sk->name, stringField(s_, *sk), sk->help); return; }
        // a too-short access-point password used to become the published default and be saved
        if (!acceptStringValue(*sk, c.text)) { printf("OUT_OF_RANGE=#P%s (%s; nothing changed)", sk->name, stringValueRule(*sk)); return; }
        Settings before = s_;
        { char v[80]; canonicalStringValue(*sk, c.text, v, sizeof v); setStringField(s_, *sk, v); }
        sanitize(s_);
        bool changed = std::memcmp(&before, &s_, sizeof s_) != 0;
        if (changed) d_.persist->requestSaveSettings();
        if (sk->secret) printf("#P%s=%s (%s; takes effect after #PRESTART)", sk->name, stringField(s_, *sk)[0] ? "(set)" : "(cleared)", changed ? "saved" : "unchanged");
        else printf("#P%s=%s (%s; takes effect after #PRESTART)", sk->name, stringField(s_, *sk), changed ? "saved" : "unchanged");
        if (std::strncmp(sk->name, "MESH", 4) == 0 && s_.meshEnabled) noteMesh();
        return;
    }
    const SettingKey* k = findSettingKey(c.key, std::strlen(c.key));
    char buf[64];
    if (!k) { printf("INVALID=#P%s (unknown; #PHELP lists the keys)", c.key); return; }
    if (!c.hasA && c.text) { printf("OUT_OF_RANGE=#P%s=%s (%s)", k->name, c.text, k->help); return; }
    if (!c.hasA) { formatSetting(s_, *k, buf, sizeof buf); printf("%s  (%s)", buf, k->help); return; }
    Settings before = s_;
    if (!k->set(s_, c.a)) { printf("OUT_OF_RANGE=#P%s%ld (%s)", k->name, static_cast<long>(c.a), k->help); return; }
    sanitize(s_);
    bool changed = std::memcmp(&before, &s_, sizeof s_) != 0;
    const char* note = "";
    if (std::strcmp(k->name, "MOTOR") == 0 && before.motor != s_.motor) {
        onMotorChanged(s_);
        note = " (new profile: LOCKED again, travel/safe-spin/min powers cleared, calibration ignored until #PSC, commissioning back to step 1)";
    } else if (std::strcmp(k->name, "INVERT") == 0) note = " (re-referenced from the limit switches)";
    else if (std::strcmp(k->name, "TRAVEL") == 0 && s_.safeSpinTicks) note = " (SAFESPIN kept: it is ticks above the bottom switch, not a share of travel)";
    else if (std::strcmp(k->name, "ROTARYTPR") == 0) note = s_.ticksPerRev > 0 ? " (marked confident: no two-turn measurement on the first rotary move)" : " (cleared: the next rotary move measures it)";
    else if (std::strcmp(k->name, "LIFTERNC") == 0 || std::strcmp(k->name, "ROTARYNC") == 0) note = " (takes effect after #PRESTART)";
    else if (std::strcmp(k->name, "BAUD") == 0 || std::strcmp(k->name, "CMDTX") == 0) {
        if (before.cmdBaud != s_.cmdBaud || before.cmdTx != s_.cmdTx) { beginCmdSerial(); note = " (command UART reopened)"; }   // a replayed #PCONFIG must not reopen it
    }
    else if (std::strcmp(k->name, "COMMISSIONED") == 0) note = s_.commissioned ? " (unlocked)" : " (LOCKED)";
    else if (std::strcmp(k->name, "WIFI") == 0 || std::strcmp(k->name, "APCHANNEL") == 0) note = " (takes effect after #PRESTART)";
    else if (std::strncmp(k->name, "MESH", 4) == 0 && std::strcmp(k->name, "MESHHB") != 0) note = " (takes effect after #PRESTART)";
    applySettings(false);
    if (changed) d_.persist->requestSaveSettings();
    formatSetting(s_, *k, buf, sizeof buf);
    printf("%s%s%s", buf, changed ? " (saved)" : " (unchanged)", note);
    if (std::strncmp(k->name, "MESH", 4) == 0 && s_.meshEnabled && changed) noteMesh();
}

// What turning the mesh on means, said before it happens (docs/PHASE5-BRIEF.md scope item 5).
void Console::noteMesh() {
    const char* ap = net_ ? net_->apSsid() : "R2Uppity-....";
    printf("MESH note: after #PRESTART the board joins the WCB mesh and stays OFF your home network; the page is then only at http://192.168.4.1 on the \"%s\" WiFi (its password as before)", ap);
    if (const char* m = meshMissing(s_)) printf("MESH note: it will not start until it has %s", m);
}

// ---------------------------------------------------------------- reports

void Console::printState() {
    Snapshot s; if (!d_.pub->read(s)) return;
    printf("STATE=%s lifter=%ld/%ld(%d%%) lifterConf=%s rotary=%ld ticks %ddeg homeConf=%s(%s) tpr=%.1f",
           stateName(static_cast<int>(s.state)), (long)s.lifterTicks, (long)s.lifterTravel, s.lifterPct, lifterConfName(static_cast<int>(s.lifterConf)),
           (long)s.rotaryTicks, s.rotaryDeg, homeConfName(static_cast<int>(s.homeConf)), homeProvName(static_cast<int>(s.homeProv)), s.ticksPerRev);
    printf("LIMITS=top:%d bot:%d home:%d diag=[%d %d] enabled=%d faults=0x%lx estop=%d(%s) override=%lus alignment=%s",
           s.topLimit, s.bottomLimit, s.homeSwitch, s.lifterDiag, s.rotaryDiag, s.enabled, (unsigned long)s.faults, s.estop,
           EstopLatch::sourceName(static_cast<EstopLatch::Source>(s.estopSource)), (unsigned long)(s.overrideRemainingMs / 1000), s.alignmentSuspect ? "SUSPECT" : "ok");
    printf("JOB=%s id=%lu last=%s %s %s (id %lu) tick=[max %luus late %lu n %lu]", jobName(static_cast<int>(s.activeJob)), (unsigned long)s.activeJobId, jobName(static_cast<int>(s.lastJob)),
           resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason), (unsigned long)s.lastProgramId, (unsigned long)s.cycleUsMax, (unsigned long)s.lateTicks, (unsigned long)s.tickCount);
    printf("SEQ=%s random=%s light=%u inputs=%u persist=%s", d_.seq->active() ? "running" : "idle", d_.dir->active() ? "on" : "off", d_.i2c->lightKit(), d_.i2c->inputs(), d_.persist->pending() ? "pending" : "saved");
}

void Console::printConfig() {
    printf("CONFIG=begin v%s schema=%u (every #P line below can be pasted back)", UPPITY_FW_VERSION, kSettingsSchema);
    char buf[64];
    for (int i = 0; i < kSettingKeyCount; ++i) { formatConfigLine(s_, kSettingKeys[i], buf, sizeof buf); line(buf); }
    if (tprUnmeasured(s_)) printf("INFO=ROTARYTPR is %.1f but not a confident measurement (v3's figure, or a spread over 5 %%): written as 0, so a restore measures it again", s_.ticksPerRev);
    printf("#PSSID=%s", s_.staSsid);
    printf("#PMESHMAC=%s", s_.meshMac);
    printf("INFO=PASS %s, APPASS %s, MESHPASS %s (passwords are never printed)", s_.staPass[0] ? "set" : "empty", std::strcmp(s_.apPass, "Astromech") == 0 ? "default" : "custom", s_.meshPass[0] ? "set" : "empty");
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
    if (cal_.bottomRest) printf("CALREST bottom=%u ticks below the bottom switch's closing point (power-up on that switch starts the count at -%u)", cal_.bottomRest, cal_.bottomRest);
    else line("CALREST not measured (calibrated before 2026-10-04): power-up on the bottom switch starts the count at 0; #PSC measures it");
    // what the jogs and nudges make of it (LifterCreep): start braking this much travel-time early
    printf("CALBRAKE down=%dms up=%dms (jogs and nudges start braking this far ahead%s)", brakeLeadMs(cal_.overDown, cal_.velDown), brakeLeadMs(cal_.overUp, cal_.velUp),
           calibrationApplies(s_, cal_) ? "" : "; not in use: the calibration is for another profile");
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
    printf("VERSION=%s phase5 build %s", UPPITY_FW_VERSION, buildId());
    printf("UPTIME=%lus commissioned=%u random=%s seq=%s persist=%s saves=%lu", static_cast<unsigned long>(up), s_.commissioned, d_.dir->active() ? "on" : "off", d_.seq->active() ? "running" : "idle", d_.persist->pending() ? "pending" : "saved", static_cast<unsigned long>(d_.persist->saves()));
    printf("SERIAL=usb+cmd@%lu id=%u dedup=%ums droppedForId=%lu duplicates=%lu cmdTx=%u", static_cast<unsigned long>(s_.cmdBaud), s_.marcId, s_.dedupMs, static_cast<unsigned long>(d_.disp->droppedForId()), static_cast<unsigned long>(d_.disp->droppedDuplicates()), s_.cmdTx);
    printf("EXPANDER=%s errors=%lu inputs=%u light=%u", d_.i2c->ok() ? "ok" : "FAILING", static_cast<unsigned long>(d_.i2c->errors()), d_.i2c->inputs(), d_.i2c->lightKit());
    if (net_) net_->printStatus(*this); else line("WIFI=off");
    if (mesh_) mesh_->printStatus(*this); else line(s_.meshEnabled ? "MESH=not running" : "MESH=off");
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
    line("WIFI=#PSSID=<home network> #PPASS=<its password> #PAPPASS=<access point password> #PWIFI0/1 #PAPCHANNEL<1-13> #PWIFIRESET (all after #PRESTART)");
    line("RESCUE=#POVERRIDE (60 s) #POVERRIDE0 #PJOGL+/- #PJOGR+/-[pct] (2 s) #PNUDGE<+-ticks> #PSETHOME");
    line("MESH=#PMESHMAC=3C:4E #PMESHPASS=<?WCBP> #PMESHQTY<n> #PMESHID<1-19> #PMESHCHAN<1-13> #PMESHCRC0/1 #PMESHHB<5-300 s> #PMESH0/1 (after #PRESTART; mesh on = no home network)");
    line("SETUP=#PCOMMISSION (status) #PCOMMISSIONSTART #PCOMMISSIONRUN #PCOMMISSIONNEXT #PCOMMISSIONSTOP #PCOMMISSIONRESET #PCOMMISSIONBYPASS #PSAFESPINHERE (step 6)");
    String keys = "KEYS=";
    for (int i = 0; i < kSettingKeyCount; ++i) { keys += "#P"; keys += kSettingKeys[i].name; keys += ' '; }
    keys += " (#PKEY<n> or #PKEY=<n> sets and saves, #PKEY reads)";
    line(keys.c_str());
}

} // namespace uppity
#endif
