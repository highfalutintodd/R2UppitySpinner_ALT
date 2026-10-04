#pragma once
#include <cstdint>
#include <cmath>
#include "../core/Hal.h"
#include "SimRng.h"
// A lead-screw lifter driven by a DC gearmotor through a TB9051FTG, stepped 1 ms at a time.
//
// Physics we model (see docs/REWRITE-PLAN.md Appendix F.8):
//  * static friction: from rest the motor only starts once the duty averaged over
//    `starvationWindowMs` reaches `breakawayDuty` (this is what starves pulsed drive on a 19:1)
//  * rotor inertia: velocity approaches the duty's target with time constant `tauDriveMs`
//  * brake (TB9051FTG "short brake": PWM1=PWM2=L or H, both low-side FETs on — confirmed in
//    datasheet Table 7.1-1) kills the rotor's momentum with `tauBrakeMs`; coast (drivers
//    disabled, outputs High-Z) decays slowly with `tauCoastMs`
//  * the screw is self-locking: once stopped it stays put with no power. While it MOVES it may
//    not be: `backdriveGravity` > 0 models a screw that back-drives under the mast's weight, so
//    a falling mast keeps going with the outputs floating (bench 2026-10-03: ~560 ticks to the
//    bottom switch after an E-STOP that switched the drivers off) while the short brake still
//    stops it in a few tens of ticks
//  * limit switches at 0 and `travelTicks`, hard stops `overTravelTicks` beyond them; the
//    velocity at the instant a switch makes is recorded as the "impact velocity"
//  * the encoder rides the motor shaft. Normally shaft forward = mast up. `mastReversed`
//    models a build where the mechanism runs the other way: the encoder still follows the
//    shaft while the mast moves opposite to it, which is why the firmware's invert setting
//    negates BOTH the drive and the count (v3's fLifterInverted did the same)
//  * encoder = shaft plus injected phantom ticks; optional frozen encoder; optional jam
namespace uppity { namespace sim {

struct LifterParams {
    double breakawayDuty = 0.45;       // fraction of full duty, window-averaged, to start from rest
    int    starvationWindowMs = 24;
    double runningFriction = 0.20;     // fraction of duty that only overcomes friction once moving
    double kv = 0.60;                  // ticks/ms at 100 % effective duty
    double tauDriveMs = 25.0;
    double tauBrakeMs = 1.5;
    double tauCoastMs = 40.0;
    // Back-drive, in ticks/ms² along the mast; both 0 = self-locking (the default). While the
    // shaft turns undriven, gravity pulls the mast down and running friction opposes the motion.
    // Neither can start it from rest or reverse it: static friction holds it once it stops.
    double backdriveGravity = 0.0;
    double backdriveFriction = 0.0;
    Ticks  travelTicks = 1350;
    Ticks  overTravelTicks = 3;
    double phantomPerMsDriving = 0.0;  // probability per ms of a spurious ±1 count
    double phantomPerMsIdle = 0.0;
    bool   mastReversed = false;       // mechanism runs backwards: shaft forward = mast down
};

class LifterModel {
  public:
    explicit LifterModel(const LifterParams& p = LifterParams(), uint32_t seed = 1) : p_(p), rng_(seed) {}

    // ---- fault injection ----
    void setJammed(bool j) { jammed_ = j; }
    void setEncoderFrozen(bool f) { encoderFrozen_ = f; }
    void setDiagFor(int ms) { diagMs_ = ms; }
    void setPosition(double ticks) { pos_ = ticks; shaft_ = mastSign() * ticks; vel_ = 0; moving_ = false; encoder_ = static_cast<Ticks>(std::floor(shaft_)); }
    void setParams(const LifterParams& p) { p_ = p; }
    const LifterParams& params() const { return p_; }

    // ---- observation ----
    double position() const { return pos_; }     // mast height in ticks
    double shaft() const { return shaft_; }       // what the encoder sees
    double velocity() const { return vel_; }      // shaft velocity
    bool   moving() const { return moving_; }
    Ticks  encoder() const { return encoder_; }
    bool   topLimit() const { return pos_ >= p_.travelTicks; }
    bool   bottomLimit() const { return pos_ <= 0.0; }
    bool   diag() const { return diagMs_ > 0; }
    double impactVelocityTop() const { return impactTop_; }
    double impactVelocityBottom() const { return impactBottom_; }
    int    hardStopHits() const { return hardStopHits_; }

    // Advance one millisecond under `cmd`. `driversEnabled` false = outputs float (coast).
    void step(BridgeCommand cmd, bool driversEnabled) {
        bool driving = driversEnabled && cmd.mode == Bridge::Drive && cmd.duty != 0 && !jammed_;
        double d = driving ? std::fabs(cmd.duty) / 1000.0 : 0.0;
        int dir = driving ? (cmd.duty > 0 ? 1 : -1) : 0;

        // starvation window
        window_[wIdx_] = d;
        wIdx_ = (wIdx_ + 1) % kWin;
        double avg = 0; int n = p_.starvationWindowMs < kWin ? p_.starvationWindowMs : kWin;
        for (int i = 0; i < n; ++i) avg += window_[(wIdx_ + kWin - 1 - i) % kWin];
        avg /= n;

        if (!moving_ && driving && avg >= p_.breakawayDuty) moving_ = true;

        if (moving_) {
            if (driving) {
                double eff = (d - p_.runningFriction) / (1.0 - p_.runningFriction);
                if (eff < 0) eff = 0;
                double vT = dir * p_.kv * eff;
                vel_ += (vT - vel_) * (1.0 / p_.tauDriveMs);
            } else {
                // Coast (drivers disabled: outputs High-Z) decays slowly; Brake (both outputs low,
                // also what a Drive with duty 0 does) kills the momentum.
                bool coasting = !driversEnabled || cmd.mode == Bridge::Coast;
                vel_ *= std::exp(-1.0 / (coasting ? p_.tauCoastMs : p_.tauBrakeMs));
                if (p_.backdriveGravity > 0 || p_.backdriveFriction > 0) {
                    double before = vel_;
                    vel_ -= mastSign() * p_.backdriveGravity;                  // mast down, in the shaft frame
                    vel_ += before > 0 ? -p_.backdriveFriction : before < 0 ? p_.backdriveFriction : 0.0;
                    if (before * vel_ <= 0) vel_ = 0;                          // stopped: static friction holds it
                }
            }
            if (!driving && std::fabs(vel_) < 0.002) { vel_ = 0; moving_ = false; }
        }
        if (jammed_) { vel_ = 0; }

        bool wasTop = topLimit(), wasBottom = bottomLimit();
        double lo = -static_cast<double>(p_.overTravelTicks), hi = p_.travelTicks + p_.overTravelTicks;
        double newPos = pos_ + mastSign() * vel_;
        if (newPos < lo || newPos > hi) {                 // hard stop: the coupled shaft stops too
            double clamped = newPos < lo ? lo : hi;
            shaft_ += mastSign() * (clamped - pos_);
            pos_ = clamped;
            if (vel_ != 0) ++hardStopHits_;
            vel_ = 0;
        } else {
            shaft_ += vel_;
            pos_ = newPos;
        }
        if (!wasTop && topLimit()) impactTop_ = std::fabs(vel_);
        if (!wasBottom && bottomLimit()) impactBottom_ = std::fabs(vel_);

        if (!encoderFrozen_) {
            encoder_ = static_cast<Ticks>(std::floor(shaft_)) + phantom_;
            double pp = driving ? p_.phantomPerMsDriving : p_.phantomPerMsIdle;
            if (pp > 0 && rng_.chance(pp)) { phantom_ += rng_.chance(0.5) ? 1 : -1; encoder_ = static_cast<Ticks>(std::floor(shaft_)) + phantom_; }
        }
        if (diagMs_ > 0) --diagMs_;
    }

  private:
    static constexpr int kWin = 64;
    double mastSign() const { return p_.mastReversed ? -1.0 : 1.0; }
    LifterParams p_;
    Rng rng_;
    double pos_ = 0, shaft_ = 0, vel_ = 0;
    bool moving_ = false, jammed_ = false, encoderFrozen_ = false;
    Ticks encoder_ = 0, phantom_ = 0;
    double window_[kWin] = {};
    int wIdx_ = 0;
    int diagMs_ = 0;
    double impactTop_ = 0, impactBottom_ = 0;
    int hardStopHits_ = 0;
};

}} // namespace uppity::sim
