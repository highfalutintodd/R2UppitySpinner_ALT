#pragma once
#include <cstdint>
#include "Units.h"
#include "Parser.h"
#include "Profiles.h"
#include "StatusSnapshot.h"
#include "TextSink.h"
// Random mode (":PM", docs/REWRITE-PLAN.md Appendix A.4): a script generator, not a motor
// driver. When the sequencer is idle and the next action is due it emits a short chain of
// ordinary commands; the sequencer runs it and the safety policy in the supervisor applies to
// every job exactly as it would to a typed one (a descent below safe height homes first, a
// rotation raises first). Aggressiveness only changes the bands and the pauses.
namespace uppity {

class RandomDirector {
  public:
    struct Limits { uint8_t lifterMinPct = 45, lifterMaxPct = 100, rotaryMinPct = 40; };

    void begin(RandomSource& rng, TextSink& out) { rng_ = &rng; out_ = &out; }

    // ":PM" (level from settings) or ":PMG/M/A".
    void startLevel(int level, Ms now) {
        param_ = false; level_ = clampv<int>(level, 0, 2); active_ = true; running_ = false; dueAt_ = now; actions_ = 0;
        out_->printf("RANDOM=on level=%d (%s)", level_, level_ == 0 ? "gentle" : level_ == 1 ? "medium" : "aggressive");
    }
    // ":PM,lift,rot,min,max" — v3's parameterised mode; missing arguments take v3's defaults.
    void startParams(const Command& c, const Limits& lim, Ms now) {
        param_ = true; active_ = true; running_ = false; dueAt_ = now; actions_ = 0;
        pLift_ = c.hasA ? clampv<int32_t>(c.a, lim.lifterMinPct, 100) : lim.lifterMinPct + 5;
        pRot_  = c.hasB ? clampv<int32_t>(c.b, lim.rotaryMinPct, 100) : lim.rotaryMinPct + 5;
        pMin_  = c.hasC ? clampv<int32_t>(c.c, 1, 600) : 1 + rng_->range(0, 4);
        pMax_  = c.hasD ? clampv<int32_t>(c.d, pMin_, 600) : pMin_ + rng_->range(0, 4);
        out_->printf("RANDOM=on lift=%d rot=%d wait=%d..%ds", pLift_, pRot_, pMin_, pMax_);
    }
    // Returns true when random mode was actually on (the console then switches a managed light off).
    bool stop(const char* why) { bool was = active_; if (was) out_->printf("RANDOM=off (%s)", why); active_ = false; running_ = false; return was; }
    bool active() const { return active_; }
    // #PRANDOMLIGHT: 0 leave the light kit alone (v3 behaviour), 1-7 set that mode when random mode
    // starts, 8 pick a new non-off mode with every action. The chains carry the light element.
    void setLightPolicy(uint8_t p) { lightPolicy_ = p > 8 ? 0 : p; }
    uint8_t lightPolicy() const { return lightPolicy_; }
    uint32_t actions() const { return actions_; }

    // Call every loop tick. Fills `out` with the next chain when one is due and the sequencer
    // is idle; returns the element count (0 = nothing to do yet).
    int next(Ms now, bool sequencerIdle, const Snapshot& s, const Limits& lim, Command* out, int cap) {
        if (!active_ || cap < 13) return 0;
        if (running_) { if (!sequencerIdle) return 0; running_ = false; dueAt_ = now + nextDelayMs_; }
        if (!sequencerIdle || static_cast<int32_t>(now - dueAt_) < 0) return 0;
        int base = 0;
        if (lightPolicy_ == 8 || (lightPolicy_ >= 1 && actions_ == 0)) {
            Command l; if (lightPolicy_ == 8) l.verb = Verb::LightRandom; else { l.verb = Verb::Light; l.a = lightPolicy_; l.hasA = true; }
            out[0] = l; base = 1;
        }
        int n = param_ ? paramAction(s, lim, out + base) : levelAction(s, lim, out + base);
        if (n > 0) { running_ = true; ++actions_; return base + n; }
        dueAt_ = now + 1000;                            // nothing suitable (e.g. big lift too close): look again soon
        return 0;
    }

  private:
    // ---- element builders ----
    static Command lift(int pct, int spd) { Command c; c.verb = Verb::LiftTo; c.a = pct; c.hasA = true; c.b = spd; c.hasB = true; return c; }
    // "home if needed": a typed ":PA0" always runs the visible settle (Todd's choice); the director's
    // homes exist for safety before a descent and are skipped when the head is already on the switch.
    static Command home() { Command c; c.verb = Verb::RotAbs; c.a = 0; c.hasA = true; c.letter = 'S'; return c; }
    static Command angle(int deg, int spd) { Command c; c.verb = Verb::RotAbs; c.a = deg; c.hasA = true; c.b = spd; c.hasB = true; return c; }
    static Command spin(int spd) { Command c; c.verb = Verb::Spin; c.a = spd; c.hasA = true; return c; }
    static Command spinStop() { Command c; c.verb = Verb::SpinStop; return c; }
    static Command waitMs(Ms ms) { Command c; c.verb = Verb::Wait; c.a = static_cast<int32_t>(ms); c.hasA = true; c.b = 2; return c; }

    int levelAction(const Snapshot& s, const Limits& lim, Command* o) {
        int liftLo, liftHi, rotLo, rotHi; float scale;
        switch (level_) {
            case 0:  liftLo = 30; liftHi = 50; rotLo = lim.rotaryMinPct + 5;  rotHi = 40;  scale = 2.0f; break;
            case 2:  liftLo = 80; liftHi = lim.lifterMaxPct > 85 ? lim.lifterMaxPct : 85; rotLo = 70; rotHi = 100; scale = 0.4f; break;
            default: liftLo = 65; liftHi = 85; rotLo = lim.rotaryMinPct + 10; rotHi = 90;  scale = 1.0f; break;
        }
        if (liftLo < lim.lifterMinPct) liftLo = lim.lifterMinPct;
        if (liftHi < liftLo) liftHi = liftLo;
        if (rotHi < rotLo) rotHi = rotLo;
        int liftSpd = rng_->range(liftLo, liftHi);
        int rotSpd = rng_->range(rotLo, rotHi);
        static const int gentle[3] = { 1, 3, 4 };
        int action = level_ == 0 ? gentle[rng_->range(0, 2)] : rng_->range(0, 5);
        int n = 0;
        Ms nextS = 2;
        switch (action) {
            case 0:   // pop up / down
                o[n++] = home(); o[n++] = lift(100, liftSpd); o[n++] = waitMs(rng_->range(500, 2000));
                o[n++] = lift(0, liftSpd); o[n++] = waitMs(rng_->range(300, 1000)); o[n++] = lift(rng_->range(70, 100), liftSpd);
                nextS = rng_->range(2, 4); break;
            case 1: { // big lift: a target at least 30 % away
                int cur = s.lifterPct, target = cur;
                for (int i = 0; i < 10; ++i) { int t = rng_->range(0, 100); if (absi(t - cur) >= 30) { target = t; break; } }
                if (absi(target - cur) < 10) return 0;
                o[n++] = home(); o[n++] = lift(target, liftSpd);
                nextS = rng_->range(2, 5); break;
            }
            case 2:   // spin at the top
                o[n++] = lift(100, liftSpd); o[n++] = spin(rng_->range(0, 1) ? rotSpd : -rotSpd);
                o[n++] = waitMs(rng_->range(2000, 5000)); o[n++] = spinStop();
                nextS = rng_->range(1, 3); break;
            case 3:   // random angle
                o[n++] = lift(100, liftSpd); o[n++] = angle(rng_->range(30, 329), lim.rotaryMinPct + 5 + rng_->range(0, 29));
                nextS = rng_->range(3, 6); break;
            case 4: { // look around
                o[n++] = lift(100, liftSpd);
                int looks = rng_->range(2, 3);
                for (int i = 0; i < looks; ++i) { o[n++] = angle(rng_->range(45, 314), lim.rotaryMinPct + 5 + rng_->range(0, 19)); o[n++] = waitMs(rng_->range(800, 2000)); }
                o[n++] = home();
                nextS = rng_->range(2, 4); break;
            }
            default: { // bob and weave
                o[n++] = home();
                int bobs = rng_->range(2, 3);
                for (int i = 0; i < bobs; ++i) { o[n++] = lift(rng_->range(70, 100), liftSpd); o[n++] = waitMs(rng_->range(200, 700)); o[n++] = lift(rng_->range(0, 19), liftSpd); o[n++] = waitMs(rng_->range(200, 700)); }
                o[n++] = lift(rng_->range(60, 100), liftSpd);
                nextS = rng_->range(1, 3); break;
            }
        }
        nextDelayMs_ = static_cast<Ms>(nextS * 1000.f * scale);
        if (nextDelayMs_ < 300) nextDelayMs_ = 300;
        out_->printf("RANDOM action=%d next=%lums", action, static_cast<unsigned long>(nextDelayMs_));
        return n;
    }

    int paramAction(const Snapshot& s, const Limits& lim, Command* o) {
        (void)s;
        int n = 0;
        switch (rng_->range(0, 2)) {
            // home first, as every level action that may descend does: the other two actions leave
            // the head turned at the top, and a turned head is held at the safe-spin height
            case 0: o[n++] = home(); o[n++] = lift(rng_->range(0, 100), pLift_); break;
            case 1: {
                int v = rng_->range(-pRot_, pRot_);
                o[n++] = lift(100, pLift_);
                if (absi(v) < lim.rotaryMinPct + 5) v = v < 0 ? -(lim.rotaryMinPct + 5) : lim.rotaryMinPct + 5;
                o[n++] = spin(v); o[n++] = waitMs(rng_->range(2000, 5000)); o[n++] = spinStop();
                break;
            }
            default: o[n++] = lift(100, pLift_); o[n++] = angle(rng_->range(0, 359), rng_->range(lim.rotaryMinPct, pRot_ > lim.rotaryMinPct ? pRot_ : lim.rotaryMinPct)); break;
        }
        nextDelayMs_ = static_cast<Ms>(rng_->range(pMin_, pMax_)) * 1000u;
        return n;
    }

    RandomSource* rng_ = nullptr; TextSink* out_ = nullptr;
    bool active_ = false, running_ = false, param_ = false;
    uint8_t lightPolicy_ = 0;
    int level_ = 1;
    int pLift_ = 50, pRot_ = 45, pMin_ = 1, pMax_ = 5;
    Ms dueAt_ = 0, nextDelayMs_ = 2000;
    uint32_t actions_ = 0;
};

} // namespace uppity
