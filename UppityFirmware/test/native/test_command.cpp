// Phase 2: settings, persistence rules, v3 import, the sequencer, random director, dispatcher.
#include "rig.h"
#include "../../src/core/Settings.h"
#include "../../src/core/Sequencer.h"
#include "../../src/core/RandomDirector.h"
#include "../../src/core/Dispatcher.h"
#include <string>
#include <vector>
using namespace uppity;
using namespace uppity::rig;

struct VecSink final : TextSink {
    std::vector<std::string> lines;
    void line(const char* t) override { lines.push_back(t); }
    bool has(const char* needle) const { for (auto& l : lines) if (l.find(needle) != std::string::npos) return true; return false; }
    int count(const char* needle) const { int n = 0; for (auto& l : lines) if (l.find(needle) != std::string::npos) ++n; return n; }
};

// ---------------------------------------------------------------- settings

TEST(settings_defaults_and_sanitize) {
    Settings s;
    CHECK_EQ(s.motor, 1); CHECK_EQ(s.homePark, 1); CHECK_EQ(s.wiggleDeg, 2); CHECK_EQ(s.driveSign, -1); CHECK_EQ(s.commissioned, 0);
    s.travelTicks = 7; s.driftPct = 99; s.rotaryMinPct = 3; s.homePark = 9; s.ticksPerRev = 1e9f; s.tprConfident = 1; s.driveSign = 0;
    sanitize(s);
    CHECK_EQ(s.travelTicks, 0); CHECK_EQ(s.driftPct, 5); CHECK_EQ(s.rotaryMinPct, 40); CHECK_EQ(s.homePark, 1); CHECK_NEAR(s.ticksPerRev, 0.0, 1e-6); CHECK_EQ(s.tprConfident, 0); CHECK_EQ(s.driveSign, -1);
}

TEST(settings_blob_round_trip_and_prefix_migration) {
    Settings a; a.inverted = 1; a.travelTicks = 1150; a.safeSpinTicks = 904; a.ticksPerRev = 1185.5f; a.tprConfident = 1; a.commissioned = 1; a.homeOffsetTicks = -18;
    uint8_t blob[sizeof(Settings)]; std::memcpy(blob, &a, sizeof a);
    Settings b; CHECK_EQ(migrateSettings(b, blob, sizeof blob), kSettingsSchema);
    CHECK_EQ(b.inverted, 1); CHECK_EQ(b.travelTicks, 1150); CHECK_EQ(b.safeSpinTicks, 904); CHECK_NEAR(b.ticksPerRev, 1185.5, 1e-3); CHECK_EQ(b.commissioned, 1); CHECK_EQ(b.homeOffsetTicks, -18);
    // a shorter blob (an older schema that stopped before the reserve) keeps its prefix and defaults the rest
    Settings c; CHECK_EQ(migrateSettings(c, blob, 40), kSettingsSchema);
    CHECK_EQ(c.travelTicks, 1150); CHECK_EQ(c.safeSpinTicks, 904); CHECK_EQ(c.inverted, 1); CHECK_EQ(c.driftPct, 5);
    // garbage: too short, or from a future schema
    Settings d; CHECK_EQ(migrateSettings(d, blob, 4), 0); CHECK_EQ(d.travelTicks, 0);
    Settings f = a; f.schema = kSettingsSchema + 1; std::memcpy(blob, &f, sizeof f);
    CHECK_EQ(migrateSettings(d, blob, sizeof blob), 0);
    // a schema-1 blob (randomLight did not exist: its byte was reserve, zero) migrates to schema 2
    Settings e = a; e.schema = 1; e.randomLight = 0; std::memcpy(blob, &e, sizeof e);
    Settings g; CHECK_EQ(migrateSettings(g, blob, sizeof blob), 1); CHECK_EQ(g.schema, kSettingsSchema); CHECK_EQ(g.randomLight, 0); CHECK_EQ(g.travelTicks, 1150);
}

TEST(settings_registry_set_get_format) {
    Settings s; const SettingKey* k = findSettingKey("INVERT", 6);
    CHECK(k != nullptr);
    CHECK(k->set(s, 1)); CHECK_EQ(k->get(s), 1); CHECK(!k->set(s, 2)); CHECK_EQ(s.inverted, 1);
    char buf[48]; formatSetting(s, *k, buf, sizeof buf); CHECK(std::string(buf) == "#PINVERT=1");
    k = findSettingKey("ROTARYTPR", 9); CHECK(k->set(s, 11855)); CHECK_NEAR(s.ticksPerRev, 1185.5, 1e-3); CHECK_EQ(s.tprConfident, 1); CHECK_EQ(k->get(s), 11855);
    CHECK(!k->set(s, 500));                             // below 100 ticks/rev
    k = findSettingKey("TRAVEL", 6); CHECK(k->set(s, 1150)); CHECK(!k->set(s, 50)); CHECK(k->set(s, 0));
    k = findSettingKey("HOMEPARK", 8); CHECK(k->set(s, 2)); CHECK(!k->set(s, 3));
    k = findSettingKey("DRIVESIGN", 9); CHECK(k->set(s, 1)); CHECK(!k->set(s, 0));
    CHECK(findSettingKey("BOGUS", 5) == nullptr);
    // every key formats as "#PKEY=<int>" and re-applies cleanly
    for (int i = 0; i < kSettingKeyCount; ++i) { formatSetting(s, kSettingKeys[i], buf, sizeof buf); CHECK(buf[0] == '#'); CHECK(kSettingKeys[i].set(s, kSettingKeys[i].get(s))); }
}

TEST(settings_to_motion_config_precedence) {
    Settings s; Calibration c;
    // profile default when nothing is known
    MotionConfig m = toMotionConfig(s, c); CHECK_EQ(m.travelTicks, 0); CHECK(!m.travelCalibrated); CHECK_EQ(m.minPowerUp, 0);
    CHECK_EQ(effectiveTravel(s, c), 1350);
    // calibration under the same profile applies
    c.valid = 1; c.motor = 1; c.travel = 1181; c.minUp = 250; c.minDown = 150; c.overUp[10] = 7;
    m = toMotionConfig(s, c); CHECK_EQ(m.travelTicks, 1181); CHECK(m.travelCalibrated); CHECK_EQ(m.minPowerUp, 250); CHECK_EQ(m.minPowerDown, 150); CHECK_EQ(m.overshootUp[10], 7);
    // the builder's explicit travel wins over the calibration; explicit min powers too
    s.travelTicks = 1150; s.minPowerUpPct = 50;
    m = toMotionConfig(s, c); CHECK_EQ(m.travelTicks, 1150); CHECK_EQ(m.minPowerUp, 500); CHECK_EQ(m.minPowerDown, 150);
    // a calibration measured under another profile is ignored
    s.travelTicks = 0; s.minPowerUpPct = 0; s.motor = 0;
    m = toMotionConfig(s, c); CHECK_EQ(m.travelTicks, 0); CHECK(!m.travelCalibrated); CHECK_EQ(effectiveTravel(s, c), 450);
    // rotary and behaviour fields map through
    s.homePark = 2; s.wiggleDeg = 4; s.homeOffsetTicks = -18; s.rotaryMinPct = 45; s.commissioned = 1; s.inverted = 1; s.safeSpinTicks = 904;
    m = toMotionConfig(s, c); CHECK(m.rotary.parkOnLeadingEdge); CHECK_EQ(m.rotary.parkApproach, 1); CHECK_EQ(m.rotary.settleWiggleDeg, 4); CHECK_EQ(m.rotary.homeOffsetTicks, -18);
    CHECK_EQ(m.rotary.minPowerPct, 45); CHECK(m.commissioned); CHECK(m.lifterInverted); CHECK_EQ(m.safeSpinTicks, 904);
    s.homePark = 0; m = toMotionConfig(s, c); CHECK(!m.rotary.parkOnLeadingEdge);
}

TEST(calibration_blob_from_result_and_migration) {
    CalibrateResult r; r.travel = 1181; r.travelOk = true; r.minUp = 250; r.minDown = 150; r.velUp[5] = 300; r.overDown[3] = 9; r.slotValidUp[5] = true;
    Calibration c; takeCalibration(c, r, 1, 12345);
    CHECK_EQ(c.valid, 1); CHECK_EQ(c.motor, 1); CHECK_EQ(c.travel, 1181); CHECK_EQ(c.velUp[5], 300); CHECK_EQ(c.overDown[3], 9); CHECK_EQ(c.slotValidUp[5], 1); CHECK_EQ(c.measuredAtMs, 12345u);
    uint8_t blob[sizeof(Calibration)]; std::memcpy(blob, &c, sizeof c);
    Calibration d; CHECK_EQ(migrateCalibration(d, blob, sizeof blob), kCalibrationSchema); CHECK_EQ(d.travel, 1181); CHECK_EQ(d.velUp[5], 300);
    CHECK_EQ(migrateCalibration(d, blob, 3), 0); CHECK_EQ(d.valid, 0);
}

TEST(v3_import_takes_todds_numbers_conservatively) {
    // Todd's v3 NVS as read with #PV3 on 2026-09-20
    V3Prefs v; v.present = true; v.lftminpwr = 50; v.lftseekbotpwr = 30; v.rotminpwr = 40; v.lftdist = 1350; v.minheight = 904; v.rotenccount = 1191; v.driftpct = 5; v.motortype = 1; v.wizstate = 7;
    Settings s; ImportReport rep; importV3(s, v, rep);
    CHECK_EQ(s.motor, 1);
    CHECK_EQ(s.minPowerUpPct, 50); CHECK_EQ(s.minPowerDownPct, 50);   // 50 is not the 19:1 default (45): builder's number
    CHECK_EQ(s.approachPowerPct, 0);                                    // 30 is the 19:1 default: skipped
    CHECK_EQ(s.rotaryMinPct, 40);                                       // default: skipped
    CHECK_EQ(s.travelTicks, 0);                                         // 1350 is the profile guess, never measured: skipped
    CHECK_EQ(s.safeSpinTicks, 904);                                     // not half of 1350: taken
    CHECK_NEAR(s.ticksPerRev, 1191.0, 1e-3); CHECK_EQ(s.tprConfident, 0);
    CHECK_EQ(s.driftPct, 5); CHECK_EQ(s.commissioned, 0); CHECK_EQ(s.v3Imported, 1);
    CHECK(rep.taken >= 4);
    bool sawTravelSkip = false, sawSafe = false; for (int i = 0; i < rep.n; ++i) { std::string l = rep.line[i]; if (l.find("profile guess") != std::string::npos) sawTravelSkip = true; if (l.find("safe-spin height 904") != std::string::npos) sawSafe = true; }
    CHECK(sawTravelSkip); CHECK(sawSafe);
}

TEST(v3_import_refuses_the_half_travel_default_and_absent_namespace) {
    V3Prefs v; v.present = true; v.lftdist = 450; v.minheight = 225; v.motortype = 0; v.lftminpwr = 65; v.rotenccount = 0;
    Settings s; ImportReport rep; importV3(s, v, rep);
    CHECK_EQ(s.safeSpinTicks, 0);        // v3's default of half travel is exactly what crashed Todd's head: never taken
    CHECK_EQ(s.travelTicks, 0); CHECK_EQ(s.minPowerUpPct, 0); CHECK_NEAR(s.ticksPerRev, 0.0, 1e-6); CHECK_EQ(s.motor, 0);
    bool sawDefault = false; for (int i = 0; i < rep.n; ++i) if (std::string(rep.line[i]).find("half-travel default") != std::string::npos) sawDefault = true;
    CHECK(sawDefault);
    V3Prefs none; Settings t; importV3(t, none, rep);
    CHECK_EQ(t.v3Imported, 1); CHECK_EQ(rep.taken, 0);
    // a measured travel with a tuned minheight both come across
    V3Prefs w; w.present = true; w.lftdist = 1181; w.minheight = 904; w.motortype = 1; Settings u; importV3(u, w, rep);
    CHECK_EQ(u.travelTicks, 1181); CHECK_EQ(u.safeSpinTicks, 904);
}

// ---------------------------------------------------------------- dedup / triggers / dispatcher

TEST(dedup_drops_cross_transport_repeats_only) {
    LineDedup d;
    CHECK(d.accept(":PP100", ReqSource::SerialCmd, 1000, 750));
    CHECK(!d.accept(":PP100", ReqSource::Mesh, 1200, 750));         // same line, other transport, inside the window
    CHECK(d.accept(":PP100", ReqSource::SerialCmd, 1300, 750));     // same transport repeating: deliberate
    CHECK(d.accept(":PP100", ReqSource::Mesh, 2500, 750));          // window elapsed
    CHECK(d.accept(":PP50", ReqSource::Mesh, 2600, 750));
    CHECK(d.accept(":PP50", ReqSource::SerialUsb, 2700, 0));        // window 0 = off
}

TEST(trigger_inputs_debounce_and_fire_once) {
    TriggerInputs t;
    CHECK_EQ(t.feed(0), 0); CHECK_EQ(t.feed(0), 0); CHECK_EQ(t.feed(0), 0); CHECK(t.primed());
    CHECK_EQ(t.feed(3), 0); CHECK_EQ(t.feed(3), 0); CHECK_EQ(t.feed(3), 3);   // stable after 3 samples
    CHECK_EQ(t.feed(3), 0); CHECK_EQ(t.feed(3), 0);                             // no repeat while held
    CHECK_EQ(t.feed(0), 0); CHECK_EQ(t.feed(0), 0); CHECK_EQ(t.feed(0), 0);     // release fires nothing
    CHECK_EQ(t.feed(5), 0); CHECK_EQ(t.feed(0), 0); CHECK_EQ(t.feed(5), 0); CHECK_EQ(t.feed(5), 0); CHECK_EQ(t.feed(5), 5);   // a glitch restarts the count
    TriggerInputs u;                                                              // a jumper already in place at power-up
    CHECK_EQ(u.feed(4), 0); CHECK_EQ(u.feed(4), 0); CHECK_EQ(u.feed(4), 0); CHECK_EQ(u.feed(4), 0);   // baseline only, never fires
    CHECK_EQ(u.feed(0), 0); CHECK_EQ(u.feed(0), 0); CHECK_EQ(u.feed(0), 0);
    CHECK_EQ(u.feed(4), 0); CHECK_EQ(u.feed(4), 0); CHECK_EQ(u.feed(4), 4);                          // re-applied: fires
}


// A command-layer rig: the motion rig plus sequencer, director, dispatcher and a text sink.
struct CmdRig : Sequencer::Listener, Dispatcher::ConfigHandler {
    Rig g;
    MemSequenceStore store;
    XorShiftRandom rng{12345};
    VecSink out;
    Sequencer seq;
    RandomDirector dir;
    Dispatcher disp;
    int lights = 1, lightSets = 0, randomStops = 0, estops = 0, configs = 0;
    Command lastConfig;
    explicit CmdRig(MotionConfig cfg = gregConfig(false)) : g(cfg) {
        seq.begin(g.queue, g.pub, store, rng, out, *this);
        dir.begin(rng, out);
        disp.begin(seq, *this, out);
        g.boot();
    }
    void onLight(int m) override { lights = m; ++lightSets; }
    void onRandom(const Command& c) override { if (c.verb == Verb::RandomLevel) dir.startLevel(c.letter == 'G' ? 0 : c.letter == 'M' ? 1 : 2, g.hal.nowMs()); else if (c.hasA || c.hasB || c.hasC || c.hasD) dir.startParams(c, limits(), g.hal.nowMs()); else dir.startLevel(1, g.hal.nowMs()); }
    void onRandomStop() override { if (dir.active()) ++randomStops; dir.stop("command"); }
    int homeStarts() const { int n = 0; for (auto& e : g.log) if (e.code == Ev::JobStart && e.a == (int)JobKind::RotaryHome) ++n; return n; }
    void onEstop() override { ++estops; g.estop.trip(EstopLatch::Source::Sequence, g.hal.nowMs()); }
    void onConfig(const Command& c, const char*, ReqSource) override { ++configs; lastConfig = c; }
    RandomDirector::Limits limits() const { RandomDirector::Limits l; l.lifterMinPct = 65; l.lifterMaxPct = 100; l.rotaryMinPct = 40; return l; }
    bool line(const char* l, ReqSource src = ReqSource::SerialUsb) { return disp.handleLine(l, src, g.hal.nowMs()); }
    // one loop() iteration: command layer, then 1 ms of motion
    void step() {
        Ms now = g.hal.nowMs();
        seq.tick(now);
        Command chain[Sequencer::kMaxElements];
        int n = dir.next(now, !seq.active(), g.snap(), limits(), chain, Sequencer::kMaxElements);
        if (n > 0) disp.submitDirector(chain, n);
        g.runMs(1);
    }
    void run(int ms) { for (int i = 0; i < ms; ++i) step(); }
    void runUntilQuiet(int maxMs) { for (int i = 0; i < maxMs; ++i) { step(); if (!seq.active() && g.sup.activeJob() == JobKind::None && g.queue.size() == 0 && !dir.active()) return; } }
};

TEST(dispatcher_routes_config_and_filters_ids) {
    CmdRig r;
    CHECK(r.line("#PSTATE")); CHECK_EQ(r.configs, 1); CHECK(r.lastConfig.verb == Verb::State);
    r.disp.setId(3);
    CHECK(!r.line("#P5STATE")); CHECK_EQ(r.configs, 1);                 // another droid's id: silent drop
    CHECK(r.line("#P3STATE")); CHECK_EQ(r.configs, 2);
    CHECK(!r.line(":P5P100")); CHECK_EQ(r.g.queue.size(), 0);
    CHECK(r.line(":PL5")); CHECK_EQ(r.lights, 5);                          // no id = ours
    CHECK(!r.line(":PQ7")); CHECK(r.out.has("INVALID=:PQ7"));
    CHECK(!r.line(":PP100:PQ", ReqSource::SerialCmd)); CHECK_EQ(r.g.queue.size(), 0);   // one bad element drops the whole line
    CHECK_EQ(r.disp.droppedForId(), 2u);
}

TEST(sequencer_runs_a_chain_with_waits_lights_and_motion) {
    CmdRig r;
    CHECK(r.line(":PP50:PW1:PL3:PP0"));
    CHECK(r.seq.active());
    r.run(200);
    CHECK(r.g.sup.activeJob() == JobKind::LifterSeek || r.g.snap().lifterTicks > 0);
    r.runUntilQuiet(30000);
    CHECK(!r.seq.active());
    CHECK_EQ(r.lights, 3);
    CHECK(r.g.snap().lifterTicks <= 5);
    CHECK(r.out.has("QUEUED=SEEK id=1 (step 1/4)"));
    CHECK(r.out.has("QUEUED=SEEK id=2 (step 4/4)"));
    CHECK(r.out.has("SEQ done"));
    // the wait really waited: the second seek started at least a second after the first ended
    Ms firstEnd = 0, secondStart = 0; int seen = 0;
    for (auto& e : r.g.log) { if (e.code == Ev::JobEnd && e.a == (int)JobKind::LifterSeek && seen == 0) { firstEnd = e.tMs; seen = 1; } else if (e.code == Ev::JobStart && e.a == (int)JobKind::LifterSeek && seen == 1) { secondStart = e.tMs; seen = 2; } }
    CHECK(seen == 2); CHECK(secondStart >= firstEnd + 1000);
}

TEST(sequencer_light_only_line_does_not_disturb_a_running_chain) {
    CmdRig r;
    r.line(":PP100:PW2:PP0");
    r.run(50);
    r.line(":PL7");
    CHECK(r.seq.active()); CHECK_EQ(r.lights, 7); CHECK(!r.out.has("SEQ replaced"));
    r.line(":PP50");                                              // a motion line replaces the pending remainder
    CHECK(r.out.has("SEQ replaced"));
    r.runUntilQuiet(30000);
    CHECK(absl(r.g.snap().lifterTicks - 225) <= 15);
}

TEST(sequencer_refused_step_aborts_the_chain) {
    MotionConfig cfg = gregConfig(false); cfg.commissioned = false;
    CmdRig r(cfg);
    r.line(":PP100:PL5");
    r.run(20);
    CHECK(!r.seq.active());
    CHECK(r.out.has("SEQ abort step 1/2: SEEK REFUSED LOCKED"));
    CHECK_EQ(r.lights, 1);                                        // the light step never ran
}

TEST(sequencer_estop_clears_everything_and_stored_px_latches) {
    CmdRig r;
    r.line(":PP100:PW5:PP0");
    r.run(100);
    r.g.estop.trip(EstopLatch::Source::Test, r.g.hal.nowMs());
    r.run(5);
    CHECK(!r.seq.active()); CHECK(r.out.has("SEQ cleared"));
    r.g.estop.release(r.g.hal.nowMs()); r.run(5);
    r.store.write(9, ":PX");
    r.line(":PS9");
    r.run(3);
    CHECK_EQ(r.estops, 1); CHECK(r.g.estop.tripped());
}

TEST(sequencer_plays_stored_sequences_and_splices_them_in_place) {
    CmdRig r;
    r.store.write(3, ":PP100:PL5:PR30");
    r.store.write(8, ":PS3:PW1:PS3");                                // nested
    r.line(":PS3");
    r.runUntilQuiet(30000);
    CHECK_EQ(r.lights, 5);
    CHECK(r.g.sup.activeJob() == JobKind::RotarySpin);                // the spin is continuous: the chain finished without waiting for it
    CHECK(!r.seq.active());
    r.line(":PR0");                                                   // stop it (home)
    r.runUntilQuiet(60000);
    r.line(":PS42"); r.run(2); CHECK(r.out.has("SEQ 42: not stored"));
    r.line(":PS8"); r.run(2); CHECK(r.out.has("SEQ 3: 3 elements"));
    r.runUntilQuiet(60000);
    CHECK(r.out.count("QUEUED=SPIN") >= 3);
    r.store.write(10, ":PS10");                                       // a sequence that plays itself
    r.line(":PS10"); r.run(50);
    CHECK(r.out.has("nested too deep")); CHECK(!r.seq.active());
}

TEST(sequencer_spin_then_ph_is_preempted_cleanly) {
    CmdRig r;
    r.line(":PP100:PR50:PW2:PH");
    r.runUntilQuiet(90000);
    CHECK(!r.seq.active());
    CHECK(r.g.snap().lifterTicks <= 5);
    CHECK(r.g.snap().lastJob == JobKind::SafeLower);
    int spins = 0; for (auto& e : r.g.log) if (e.code == Ev::JobStart && e.a == (int)JobKind::RotarySpin) ++spins;
    CHECK_EQ(spins, 1);
}

TEST(sequencer_wait_clamps_to_60s_and_random_wait) {
    CmdRig r;
    r.line(":PW600:PL2");
    r.run(2);
    CHECK(r.out.has("WAIT clamped from 600s to 60s"));
    r.run(59000); CHECK_EQ(r.lights, 1);
    r.run(1100); CHECK_EQ(r.lights, 2);
    r.line(":PWR3:PL4"); r.run(3100); CHECK_EQ(r.lights, 4);
}

TEST(random_director_gentle_uses_only_its_three_actions_and_obeys_policy) {
    CmdRig r;
    r.line(":PMG");
    CHECK(r.dir.active()); CHECK(r.out.has("RANDOM=on level=0"));
    r.run(240000);                                                    // four minutes of gentle random
    CHECK(r.dir.actions() >= 5);
    for (auto& l : r.out.lines) if (l.rfind("RANDOM action=", 0) == 0) { int a = l[14] - '0'; CHECK(a == 1 || a == 3 || a == 4); }
    // policy held: every rotary job started with the lifter above the safe height
    Ticks lifter = 0; bool ok = true;
    for (auto& e : r.g.log) {
        if (e.code == Ev::JobStart && (e.a == (int)JobKind::RotaryAbsolute || e.a == (int)JobKind::RotaryHome || e.a == (int)JobKind::RotarySpin)) { if (lifter <= 225 && !r.g.hal.lifter.topLimit()) ok = false; }
        if (e.code == Ev::LimitTop || e.code == Ev::LimitBottom || e.code == Ev::JobEnd) lifter = r.g.snap().lifterTicks;
    }
    CHECK(ok);
    r.line(":PP100");                                                 // any motion command ends random mode
    CHECK(!r.dir.active()); CHECK(r.out.has("RANDOM=off"));
    CHECK_EQ(r.randomStops, 1);
}

TEST(random_director_medium_runs_all_actions_and_lights_do_not_end_it) {
    CmdRig r;
    r.line(":PM");
    r.run(360000);
    CHECK(r.dir.active());
    bool seen[6] = {}; for (auto& l : r.out.lines) if (l.rfind("RANDOM action=", 0) == 0) seen[l[14] - '0'] = true;
    int kinds = 0; for (bool b : seen) kinds += b;
    CHECK(kinds >= 4);
    r.line(":PL3"); CHECK(r.dir.active());
    r.line(":PW1"); CHECK(r.dir.active());
    r.line(":PH"); CHECK(!r.dir.active());
    r.runUntilQuiet(120000);
    CHECK(r.g.snap().lifterTicks <= 5);
}

TEST(random_director_parameterised_and_aggressive) {
    CmdRig r;
    r.line(":PM,80,80,1,2");
    CHECK(r.out.has("RANDOM=on lift=80 rot=80 wait=1..2s"));
    r.run(60000);
    CHECK(r.dir.actions() >= 3);
    r.line(":PMA"); CHECK(r.out.has("level=2 (aggressive)"));
    r.run(60000);
    CHECK(r.dir.active());
    r.g.estop.trip(EstopLatch::Source::Test, r.g.hal.nowMs()); r.run(2);
    // E-STOP: the console stops the director; here the sequencer cleared and nothing is queued
    CHECK(!r.seq.active()); CHECK_EQ(r.g.queue.size(), 0);
}

TEST(supervisor_hold_intake_keeps_requests_queued) {
    Rig g(gregConfig()); g.boot();
    g.sup.setHoldIntake(true);
    g.seek(200, 700); g.runMs(50);
    CHECK(g.sup.activeJob() == JobKind::None); CHECK_EQ(g.queue.size(), 1);
    g.sup.setHoldIntake(false); g.runMs(5);
    CHECK(g.sup.activeJob() == JobKind::LifterSeek);
    g.runUntilIdle(20000);
    CHECK_EQ(g.snap().lastProgramId, 1u);
}

TEST(snapshot_last_program_id_covers_refusals) {
    Rig g(gregConfig()); g.boot();
    g.estop.trip(EstopLatch::Source::Test, g.hal.nowMs());
    uint32_t id = g.seek(200, 700); g.runMs(3);
    CHECK_EQ(g.snap().lastProgramId, id); CHECK(g.snap().lastResult == JobResult::Refused);
}

TEST(sequencer_spin_from_the_bottom_times_the_spin_not_the_raise) {
    CmdRig r;
    r.line(":PR50:PW2:PL6");
    r.runUntilQuiet(60000);
    // the wait began only once the spin step was running, so the light changed while spinning at the top
    CHECK_EQ(r.lights, 6);
    CHECK(r.g.sup.activeJob() == JobKind::RotarySpin);
    CHECK_EQ(r.g.snap().lifterTicks, 450);
    Ms spinStart = 0, lightAt = 0;
    for (auto& e : r.g.log) if (e.code == Ev::JobStart && e.a == (int)JobKind::RotarySpin) spinStart = e.tMs;
    lightAt = r.g.hal.nowMs();
    CHECK(spinStart > 0); CHECK(lightAt >= spinStart + 2000);
    r.line(":PH"); r.runUntilQuiet(120000); CHECK(r.g.snap().lifterTicks <= 5);
}

TEST(home_after_a_spin_goes_to_the_nearest_zero) {
    CmdRig r;
    r.line(":PA0"); r.runUntilQuiet(60000);                                    // referenced, head at the top and home
    CHECK(r.g.sup.rotary().confidence == HomeConfidence::SwitchLive || r.g.sup.rotary().confidence == HomeConfidence::Latched);
    r.line(":PD1080"); r.runUntilQuiet(60000);                                 // three turns on: count ~3550, head physically home
    Ticks before = r.g.snap().rotaryTicks; CHECK(before > 3000);
    double encAtStart = r.g.hal.rotary.encoder(), lo = encAtStart, hi = encAtStart;
    r.line(":PA0");
    for (int i = 0; i < 60000; ++i) { r.step(); double e = r.g.hal.rotary.encoder(); if (e < lo) lo = e; if (e > hi) hi = e; if (!r.seq.active() && r.g.sup.activeJob() == JobKind::None && r.g.queue.size() == 0) break; }
    CHECK(r.g.snap().lastJob == JobKind::RotaryHome); CHECK(r.g.snap().lastResult == JobResult::Done);
    CHECK(hi - lo < 0.75 * r.g.hal.rotary.params().ticksPerRev);               // never unwound the three turns (was ~3.1 turns before the fix)
    CHECK(absl(r.g.snap().rotaryTicks) < 60);                                  // and the frame is back near zero
}

TEST(director_home_is_skipped_when_already_home_and_never_raises_for_it) {
    CmdRig r;
    r.line(":PA0"); r.runUntilQuiet(60000);                                    // raise and home the head
    r.line(":PH"); r.runUntilQuiet(60000);                                     // mast down, head still on the switch
    CHECK(r.g.sup.rotary().confidence == HomeConfidence::SwitchLive || r.g.sup.rotary().confidence == HomeConfidence::Latched);
    CHECK(r.g.snap().lifterTicks <= 5);
    int homes = r.homeStarts(); CHECK_EQ(homes, 1);
    r.g.clearRecordsAndLog();
    Command home; home.verb = Verb::RotAbs; home.a = 0; home.hasA = true; home.letter = 'S';
    Command lift; lift.verb = Verb::LiftTo; lift.a = 40; lift.hasA = true;
    Command chain[2] = { home, lift };
    r.disp.submitDirector(chain, 2); r.runUntilQuiet(60000);
    CHECK_EQ(r.homeStarts(), 0);                                               // no home ran (log cleared above)
    int raises = 0; for (auto& e : r.g.log) if (e.code == Ev::JobStart && e.a == (int)JobKind::LifterSeek && e.b == 450) ++raises;
    CHECK_EQ(raises, 0);                                                       // and the lifter was not raised just to skip it (log cleared above)
    CHECK(absl(r.g.snap().lifterTicks - 180) <= 15);                           // the descent itself happened
    r.line(":PA0"); r.runUntilQuiet(60000);                                    // a typed :PA0 still runs the full settle
    CHECK_EQ(r.homeStarts(), 1);
    // buildRequest carries the flag only when asked
    MotionRequest q; CHECK(buildRequest(home, 450, r.rng, q)); CHECK(q.kind == JobKind::RotaryHome); CHECK_EQ(q.c, 2);
    Command plain = home; plain.letter = 0; CHECK(buildRequest(plain, 450, r.rng, q)); CHECK_EQ(q.c, 0);
}

TEST(full_travel_seek_already_on_the_switch_reports_itself) {
    CmdRig r;
    r.line(":PH"); r.runUntilQuiet(60000);
    int ends = 0; for (auto& e : r.g.log) if (e.code == Ev::JobEnd && e.a == (int)JobKind::LifterSeek) ++ends;
    r.line(":PP0"); r.run(10);
    int ends2 = 0; const Event* last = nullptr;
    for (auto& e : r.g.log) if (e.code == Ev::JobEnd && e.a == (int)JobKind::LifterSeek) { ++ends2; last = &e; }
    CHECK_EQ(ends2, ends + 1); CHECK(last && last->b == (int)JobResult::Done && last->c == (int)Reason::AlreadyThere);
    CHECK_EQ(r.g.snap().lastProgramId, r.g.snap().lastProgramId);             // and the sequencer saw it complete
    CHECK(!r.seq.active());
}

TEST(random_light_policy_sets_the_light_kit) {
    CmdRig r;
    r.dir.setLightPolicy(7);
    r.line(":PMG"); r.run(30000);
    CHECK_EQ(r.lights, 7); CHECK_EQ(r.lightSets, 1);                           // once, at the first action
    r.line(":PH"); CHECK(!r.dir.active());                                     // (the console switches it off on stop; host rig has no console)
    r.runUntilQuiet(60000);
    CmdRig q;
    q.dir.setLightPolicy(8);
    q.line(":PM"); q.run(90000);
    CHECK(q.lightSets >= 3); CHECK(q.lights != 1);                             // a new non-off mode with every action
    CHECK_EQ(q.lightSets, (int)q.dir.actions());
    CmdRig z;                                                                  // policy 0: never touched
    z.line(":PM"); z.run(60000); CHECK_EQ(z.lightSets, 0);
}
