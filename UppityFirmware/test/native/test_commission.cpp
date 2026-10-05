// Phase 4: the new #P grammar, settings schema 4, the replayable #PCONFIG order, the jog powers,
// and the commissioning flow run end to end on the simulated periscope.
#include "rig.h"
#include "../../src/core/Settings.h"
#include "../../src/core/Sequencer.h"
#include "../../src/core/RandomDirector.h"
#include "../../src/core/Commissioning.h"
#include <string>
#include <vector>
using namespace uppity;
using namespace uppity::rig;

namespace {
struct Lines final : TextSink {
    std::vector<std::string> v;
    void line(const char* t) override { v.push_back(t); }
    bool has(const char* needle) const { for (auto& l : v) if (l.find(needle) != std::string::npos) return true; return false; }
};
Command cmdOf(const char* s) { Command c; parseLine(s, c); return c; }
}

// ---------------------------------------------------------------- grammar

TEST(parser_phase4_rescue_and_commissioning_verbs) {
    Command c = cmdOf("#POVERRIDE"); CHECK(c.verb == Verb::Override); CHECK_EQ(c.a, 1);
    c = cmdOf("#POVERRIDE0"); CHECK(c.verb == Verb::Override); CHECK_EQ(c.a, 0);
    c = cmdOf("#POVERRIDE1"); CHECK(c.verb == Verb::Override); CHECK_EQ(c.a, 1);
    CHECK(cmdOf("#POVERRIDE2").verb == Verb::Invalid);
    c = cmdOf("#PJOGL+"); CHECK(c.verb == Verb::Jog); CHECK(c.letter == 'L'); CHECK_EQ(c.b, 1); CHECK_EQ(c.a, 0);
    c = cmdOf("#PJOGR-35"); CHECK(c.verb == Verb::Jog); CHECK(c.letter == 'R'); CHECK_EQ(c.b, -1); CHECK_EQ(c.a, 35);
    c = cmdOf("#PJOG0"); CHECK(c.verb == Verb::Jog); CHECK(c.letter == 0);
    CHECK(cmdOf("#PJOGL").verb == Verb::Invalid); CHECK(cmdOf("#PJOGX+5").verb == Verb::Invalid); CHECK(cmdOf("#PJOGL+150").verb == Verb::Invalid);
    c = cmdOf("#PNUDGE-12"); CHECK(c.verb == Verb::Nudge); CHECK_EQ(c.a, -12);
    c = cmdOf("#PNUDGE+30"); CHECK(c.verb == Verb::Nudge); CHECK_EQ(c.a, 30);
    CHECK(cmdOf("#PNUDGE0").verb == Verb::Invalid); CHECK(cmdOf("#PNUDGE").verb == Verb::Invalid);
    CHECK(cmdOf("#PSETHOME").verb == Verb::SetHome);
    CHECK(cmdOf("#PSAFESPINHERE").verb == Verb::SafeSpinHere);
    c = cmdOf("#PSAFESPIN904"); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "SAFESPIN"); CHECK_EQ(c.a, 904);   // still the settings key
    c = cmdOf("#PCOMMISSION"); CHECK(c.verb == Verb::Commission); CHECK(c.letter == 0);
    CHECK(cmdOf("#PCOMMISSIONSTART").letter == 'S'); CHECK(cmdOf("#PCOMMISSIONSTOP").letter == 'P'); CHECK(cmdOf("#PCOMMISSIONRUN").letter == 'R');
    CHECK(cmdOf("#PCOMMISSIONNEXT").letter == 'N'); CHECK(cmdOf("#PCOMMISSIONRESET").letter == 'X'); CHECK(cmdOf("#PCOMMISSIONBYPASS").letter == 'B');
    CHECK(cmdOf("#PCOMMISSIONFOO").verb == Verb::Invalid);
    c = cmdOf("#PCOMMISSIONED=2"); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "COMMISSIONED"); CHECK_EQ(c.a, 2);
    c = cmdOf("#P3COMMISSIONRUN"); CHECK(c.verb == Verb::Commission); CHECK_EQ(c.id, 3);
    // none of them is a chain element: a stored sequence can never carry an override or a jog
    Command els[4]; char bad[32];
    CHECK_EQ(Sequencer::parseChain(":PP100:#POVERRIDE", els, 4, bad, sizeof bad), -1);
}

// ---------------------------------------------------------------- settings

TEST(settings_schema4_carves_commission_step_from_the_reserve) {
    static_assert(sizeof(Settings) >= 224, "layout");                // schema 5 (phase 5) appended the mesh fields
    Settings s; CHECK_EQ(s.commissionStep, 0); CHECK(kSettingsSchema >= 4);
    // a schema-3 blob: the byte that is now commissionStep was reserve, i.e. zero
    Settings old; old.schema = 3; old.travelTicks = 1150; old.safeSpinTicks = 904; old.commissioned = 1; old.randomLight = 7; old.theme = 1;
    std::snprintf(old.staSsid, sizeof old.staSsid, "WordNet");
    uint8_t blob[sizeof(Settings)]; std::memcpy(blob, &old, sizeof old);
    Settings m; CHECK_EQ(migrateSettings(m, blob, sizeof blob), 3);
    CHECK_EQ(m.schema, kSettingsSchema); CHECK_EQ(m.commissionStep, 0); CHECK_EQ(m.travelTicks, 1150); CHECK_EQ(m.safeSpinTicks, 904);
    CHECK_EQ(m.commissioned, 1); CHECK_EQ(m.randomLight, 7); CHECK_EQ(m.theme, 1); CHECK(std::string(m.staSsid) == "WordNet");
    s.commissionStep = 42; sanitize(s); CHECK_EQ(s.commissionStep, 0);
    s.commissionStep = 6; sanitize(s); CHECK_EQ(s.commissionStep, 6);
}

TEST(settings_config_dump_replays_with_the_motor_first) {
    CHECK(std::string(kSettingKeys[0].name) == "MOTOR");
    // Todd's board, dumped...
    Settings a; a.motor = 1; a.inverted = 1; a.travelTicks = 1150; a.safeSpinTicks = 904; a.ticksPerRev = 1185.5f; a.tprConfident = 1;
    a.minPowerUpPct = 50; a.minPowerDownPct = 50; a.randomLight = 7; a.commissioned = 1; a.wiggleDeg = 2;
    std::vector<std::string> dump;
    for (int i = 0; i < kSettingKeyCount; ++i) { char buf[64]; formatSetting(a, kSettingKeys[i], buf, sizeof buf); dump.push_back(buf); }
    // ...replayed into a board on another profile, line by line, with the console's side effect for MOTOR
    Settings b; b.motor = 0;
    for (auto& l : dump) {
        Command c; CHECK(parseLine(l.c_str(), c)); CHECK(c.verb == Verb::Set); CHECK(c.hasA);
        const SettingKey* k = findSettingKey(c.key, std::strlen(c.key)); CHECK(k != nullptr); if (!k) continue;
        Settings before = b; CHECK(k->set(b, c.a)); sanitize(b);
        if (std::strcmp(k->name, "MOTOR") == 0 && before.motor != b.motor) onMotorChanged(b);
    }
    for (int i = 0; i < kSettingKeyCount; ++i) CHECK_EQ(kSettingKeys[i].get(b), kSettingKeys[i].get(a));   // nothing the motor line cleared was lost
}

TEST(settings_motor_change_resets_what_it_must) {
    Settings s; s.commissioned = 2; s.travelTicks = 1150; s.safeSpinTicks = 904; s.minPowerUpPct = 50; s.approachPowerPct = 30; s.commissionStep = 5; s.inverted = 1;
    onMotorChanged(s);
    CHECK_EQ(s.commissioned, 0); CHECK_EQ(s.travelTicks, 0); CHECK_EQ(s.safeSpinTicks, 0); CHECK_EQ(s.minPowerUpPct, 0); CHECK_EQ(s.approachPowerPct, 0);
    CHECK_EQ(s.commissionStep, 1); CHECK_EQ(s.inverted, 1);                         // back to step 1; the wiring fact stays
    Settings t; onMotorChanged(t); CHECK_EQ(t.commissionStep, 0);                   // never started: stays not started
}

TEST(power_set_for_todds_numbers) {
    Settings s; s.motor = 1; s.minPowerUpPct = 50; s.minPowerDownPct = 50; s.rotaryMinPct = 40;
    Calibration c; c.valid = 1; c.motor = 1; c.travel = 1179; c.minUp = 250; c.minDown = 150;
    PowerSet p = powerSet(s, c);
    CHECK_EQ(p.floorUp, 500); CHECK_EQ(p.floorDown, 500);                             // #PMINUP/#PMINDOWN win for seeks
    CHECK_EQ(p.slowUp, 250); CHECK_EQ(p.slowDown, 150);                               // the jog creeps at the calibrated minimum
    CHECK_EQ(p.cap, 750);                                                             // 19:1 ceiling
    CHECK_EQ(p.rotaryMinPct, 40); CHECK_EQ(p.rotarySlowPct, 26);
    MotionConfig m = toMotionConfig(s, c); CHECK_EQ(m.jogPowerUp, 250); CHECK_EQ(m.jogPowerDown, 150);
    c.motor = 0; p = powerSet(s, c);                                                  // a calibration from another profile does not count
    CHECK_EQ(p.slowUp, 450); CHECK_EQ(p.slowDown, 450);
    s.expertMode = 1; CHECK_EQ(powerSet(s, c).cap, 1000);
    s.rotaryMinPct = 15; CHECK_EQ(powerSet(s, c).rotarySlowPct, 12);
}

// ---------------------------------------------------------------- the commissioning flow on the simulator

namespace {
// The simulated periscope plus the command layer, with the flow's host implemented the way the
// console does it on the board. Starts LOCKED (commissioned = 0) with the rig's Greg fixture.
struct FlowRig : Sequencer::Listener {
    Rig g;
    MemSequenceStore store; XorShiftRandom rng{99}; Lines out;
    Sequencer seq; RandomDirector dir;
    Settings s; Calibration cal;
    CommissionFlow flow;
    size_t fed = 0;
    int saves = 0;

    struct H final : CommissionFlow::Host {
        FlowRig* r;
        Settings& settings() override { return r->s; }
        const Calibration& calibration() override { return r->cal; }
        const CharacteriseResult& characterisation() override { return r->g.sup.lastCharacterisation(); }
        void settingsChanged() override { r->apply(); ++r->saves; }
        uint32_t pushJob(const MotionRequest& q) override { r->dir.stop("commissioning"); r->seq.clear(nullptr); return r->g.queue.push(q); }
        bool runChain(const Command* els, int n) override { return r->seq.submit(els, n, ReqSource::Commission, false); }
        bool chainActive() override { return r->seq.active(); }
        void cancelMotion() override { r->dir.stop("x"); r->seq.clear(nullptr); r->g.queue.cancel(); }
        void randomStart(int level) override { r->dir.startLevel(level, r->g.hal.nowMs()); }
        void randomStop() override { r->dir.stop("commissioning"); }
        uint32_t randomActions() override { return r->dir.actions(); }
        bool expanderOk() override { return true; }
    } host;

    explicit FlowRig(bool reversed = false, double startTicks = 0) : g(gregConfig(false)) {
        host.r = this;
        s.motor = 0; s.travelTicks = 450; s.safeSpinTicks = 225; s.ticksPerRev = 1184.6f; s.tprConfident = 1; s.driveSign = 1; s.commissioned = 0;
        if (reversed) g.hal.lifter.setParams([]{ sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; p.mastReversed = true; return p; }());
        g.hal.lifter.setPosition(startTicks); g.setRotaryAngleFromCentre(300);
        apply();
        seq.begin(g.queue, g.pub, store, rng, out, *this);
        dir.begin(rng, out);
        flow.begin(host, out);
        g.boot();
    }
    void apply() { MotionConfig m = toMotionConfig(s, cal); m.commissioningActive = flow.active(); g.sup.setConfig(m); }
    void onLight(int) override {}
    void onRandom(const Command&) override {}
    void onRandomStop() override { dir.stop("command"); }
    void onEstop() override {}
    // one loop(): the console's order (results kept, flow, director, events), then 1 ms of motion
    void step() {
        Ms now = g.hal.nowMs();
        if (g.sup.takeCalibrationUpdated()) takeCalibration(cal, g.sup.lastCalibration(), s.motor, now);
        if (g.sup.takeCharacterisationUpdated()) takeCharacterisation(s, g.sup.config().rotary);
        seq.tick(now);
        flow.update(g.snap(), now);
        Command chain[Sequencer::kMaxElements];
        RandomDirector::Limits lim; lim.lifterMinPct = 65; lim.lifterMaxPct = 100; lim.rotaryMinPct = 40;
        int n = dir.next(now, !seq.active(), g.snap(), lim, chain, Sequencer::kMaxElements);
        if (n > 0) seq.submit(chain, n, ReqSource::Random, true);
        g.runMs(1);
        for (; fed < g.log.size(); ++fed) flow.onEvent(g.log[fed]);
    }
    void run(int ms) { for (int i = 0; i < ms; ++i) step(); }
    // run until the flow's step run ends (passed / failed), with a bound
    bool runStep(int maxMs) {
        if (!flow.runStep(g.snap(), g.hal.nowMs())) return false;
        for (int i = 0; i < maxMs && flow.run() == CommissionFlow::Run::Busy; ++i) step();
        run(20);
        return flow.run() == CommissionFlow::Run::Passed;
    }
    void quiet(int maxMs) { for (int i = 0; i < maxMs; ++i) { step(); if (g.sup.activeJob() == JobKind::None && g.queue.size() == 0 && !seq.active()) break; } run(5); }
    uint32_t push(JobKind k, int32_t a, int32_t b, int32_t c) { MotionRequest r; r.kind = k; r.a = a; r.b = b; r.c = c; r.source = ReqSource::Web; return g.queue.push(r); }
};
}

TEST(commissioning_locked_board_moves_only_while_the_flow_is_active) {
    FlowRig r;
    CHECK(r.g.snap().state == MotionState::Locked);
    r.push(JobKind::LifterSeek, 450, 0, 1); r.quiet(2000);
    CHECK(r.g.snap().lastResult == JobResult::Refused);                            // locked
    CHECK(r.flow.start(r.g.snap())); r.run(3);
    CHECK(r.flow.active()); CHECK_EQ(r.s.commissionStep, CommissionFlow::Profile); CHECK(r.g.snap().commissioning);
    CHECK(!r.flow.runStep(r.g.snap(), r.g.hal.nowMs()));                            // nothing to run in step 1
    CHECK(!r.flow.safeSpinHere(r.g.snap()));                                         // and never outside step 6
    CHECK(r.out.has("SAFESPINHERE=refused"));
    r.g.estop.trip(EstopLatch::Source::Web, r.g.hal.nowMs()); r.run(5);             // an E-STOP pauses it
    CHECK(!r.flow.active()); CHECK(r.out.has("COMMISSION=paused at step 1 (E-STOP)"));
    CHECK(!r.flow.start(r.g.snap()));                                                // and it cannot resume while latched
    r.g.estop.release(r.g.hal.nowMs()); r.run(5);
    r.push(JobKind::LifterSeek, 450, 0, 1); r.quiet(2000);
    CHECK(r.g.snap().lastResult == JobResult::Refused);                            // paused = locked again
    CHECK(r.flow.start(r.g.snap())); r.run(3);
    CHECK_EQ(r.s.commissionStep, CommissionFlow::Profile);                           // resumed where it was
}

TEST(commissioning_runs_every_step_on_the_simulator) {
    FlowRig r(false, 0);
    CHECK(r.flow.start(r.g.snap())); r.run(3);
    CHECK(r.flow.next(r.g.snap()));                                                  // 1 profile (the page's acknowledgements)
    CHECK(r.flow.next(r.g.snap()));                                                  // 2 wiring: no contradiction, no fault
    CHECK_EQ(r.s.commissionStep, CommissionFlow::Creep);
    CHECK(!r.flow.next(r.g.snap()));                                                 // 3 must pass first
    CHECK(r.runStep(120000));                                                        // 3: the first motion, a creep up to the top switch
    CHECK(r.out.has("reached the top switch"));
    CHECK(r.g.hal.lifter.topLimit());
    CHECK(r.flow.detail() > 400);                                                    // the travel the creep saw from the bottom switch
    CHECK(r.flow.next(r.g.snap()));
    CHECK(r.runStep(200000));                                                        // 4 rotary: raise, three turns, home
    CHECK(r.g.hal.lifter.topLimit());
    CHECK(r.g.snap().homeConf == HomeConfidence::Latched || r.g.snap().homeConf == HomeConfidence::SwitchLive);
    CHECK(r.flow.next(r.g.snap()));
    CHECK(r.runStep(900000));                                                        // 5 calibration
    CHECK(r.cal.valid); CHECK(r.out.has("(stored)"));
    CHECK(r.flow.next(r.g.snap()));
    CHECK_EQ(r.s.commissionStep, CommissionFlow::SafeSpin);
    // 6: RUN clears the old height, raises to the top and turns the head there
    r.flow.runStep(r.g.snap(), r.g.hal.nowMs());
    CHECK_EQ(r.s.safeSpinTicks, 0); CHECK_EQ(r.flow.previousSafeSpin(), 225);
    for (int i = 0; i < 120000 && r.flow.run() == CommissionFlow::Run::Busy; ++i) r.step();
    CHECK(r.flow.run() == CommissionFlow::Run::Idle);
    CHECK(r.g.hal.lifter.topLimit());
    int deg = r.g.snap().rotaryDeg; CHECK(deg > 70 && deg < 110);
    CHECK(!r.flow.safeSpinHere(r.g.snap()));                                         // still at the top
    // the operator: override on, then small steps down while watching
    r.push(JobKind::LifterCreep, -30, 0, 2); r.quiet(3000);                         // no override: nothing moves
    CHECK(r.g.hal.lifter.topLimit());
    r.g.queue.setOverride(r.g.hal.nowMs() + CommandQueue::kOverrideMs);
    for (int i = 0; i < 3; ++i) { r.push(JobKind::LifterCreep, -30, 0, 2); r.quiet(3000); }
    Ticks h = r.g.snap().lifterTicks;
    CHECK(h < 400); CHECK(h > 300);
    CHECK(r.flow.safeSpinHere(r.g.snap()));
    CHECK_EQ(r.s.safeSpinTicks, h + 20);                                             // 3 % of 450 is under the 20-tick minimum margin
    CHECK(r.flow.next(r.g.snap()));
    r.g.queue.setOverride(0); r.run(3);
    // 7 acceptance, starting with the head still turned below the new height: its first home raises first
    CHECK(r.runStep(600000));
    CHECK(r.out.has("RANDOM=on level=0"));
    CHECK(!r.dir.active());
    CHECK(r.flow.next(r.g.snap()));
    CHECK_EQ(r.s.commissioned, 2); CHECK_EQ(r.s.commissionStep, CommissionFlow::Complete); CHECK(!r.flow.active());
    r.run(3);
    CHECK(r.g.snap().state != MotionState::Locked);
    CHECK(r.out.has("COMMISSIONED=2"));
}

TEST(commissioning_creep_test_catches_a_reversed_mechanism) {
    FlowRig r(true, 200);                                                            // reversed, mid-travel: height unknown
    CHECK(r.flow.start(r.g.snap())); r.run(3);
    r.flow.next(r.g.snap()); r.flow.next(r.g.snap());
    CHECK(!r.runStep(60000));
    CHECK(r.flow.run() == CommissionFlow::Run::Failed);
    CHECK(std::string(r.flow.message()).find("#PINVERT") != std::string::npos);
    CHECK(r.g.hal.lifter.position() > -4);                                           // stopped on the switch, not in the stop
    CHECK(!r.flow.next(r.g.snap()));
    r.s.inverted = 1; r.apply(); r.run(5);                                           // the builder flips "runs backwards"
    CHECK(r.runStep(120000));
    CHECK(r.flow.next(r.g.snap()));
}

TEST(commissioning_safe_spin_here_needs_a_turned_head_below_the_top) {
    FlowRig r(false, 450);
    r.flow.start(r.g.snap()); r.run(3);
    r.s.commissionStep = CommissionFlow::SafeSpin; r.apply(); r.run(3);
    r.push(JobKind::RotaryHome, 0, 0, 0); r.quiet(60000);
    CHECK(!r.flow.safeSpinHere(r.g.snap())); CHECK(r.out.has("mast is still at the top"));
    r.push(JobKind::LifterSeek, 300, 650, 0); r.quiet(20000);                       // head home, lowered: allowed, but not a measurement
    CHECK(!r.flow.safeSpinHere(r.g.snap())); CHECK(r.out.has("within 45 degrees of home"));
    CHECK_EQ(r.s.safeSpinTicks, 225);
    CHECK(!r.flow.next(r.g.snap()));
}

TEST(commissioning_next_after_a_motor_change_starts_over) {
    FlowRig r;
    r.flow.start(r.g.snap()); r.run(3);
    r.flow.next(r.g.snap()); r.flow.next(r.g.snap());
    CHECK_EQ(r.s.commissionStep, CommissionFlow::Creep);
    onMotorChanged(r.s); r.apply();
    CHECK_EQ(r.s.commissionStep, CommissionFlow::Profile);
    r.flow.reset(); CHECK_EQ(r.s.commissionStep, CommissionFlow::Profile);
    r.flow.stop("test"); r.flow.reset(); CHECK_EQ(r.s.commissionStep, CommissionFlow::NotStarted);
    r.s.commissionStep = CommissionFlow::Complete;
    CHECK(!r.flow.start(r.g.snap())); CHECK(r.out.has("already complete"));
    r.flow.bypass(); CHECK_EQ(r.s.commissioned, 1);
}
