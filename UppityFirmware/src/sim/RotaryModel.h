#pragma once
#include <cstdint>
#include <cmath>
#include "../core/Hal.h"
#include "SimRng.h"
// The periscope head's rotary drive plus its spherical-cam home switch, stepped 1 ms at a time.
//
//  * the drivetrain needs `breakawayDuty` for `breakawayDwellMs` continuous to start from rest,
//    which is what makes the smallest controllable move a burst rather than a tick
//  * velocity/brake/coast as the lifter, without a self-locking screw (coast is slower)
//  * the home switch conducts solidly within `halfWidthRest` ticks of the cam centre, and only
//    while moving in the marginal band out to `halfWidthMoving`, where the contact chatters with
//    probability `chatterP` per ms. At rest inside the band it reads OPEN. First contact is on
//    whichever band edge the head approaches from: direction-dependent by geometry.
//  * faults: stuck closed, disconnected, wrong polarity, jam, DIAG, frozen encoder, phantoms
namespace uppity { namespace sim {

struct RotaryParams {
    double ticksPerRev = 1184.6;
    double breakawayDuty = 0.30;
    int    breakawayDwellMs = 8;
    double runningFriction = 0.15;
    double kv = 1.40;                 // ticks/ms at 100 % effective duty
    double tauDriveMs = 15.0;
    double tauBrakeMs = 2.0;
    double tauCoastMs = 30.0;
    double camCentreTicks = 300.0;    // where the switch centre sits in the model's own frame
    double halfWidthRest = 2.0;       // solid conduction zone (2–7 ticks conducting at rest)
    double halfWidthMoving = 8.0;     // marginal band while moving (~14–17 ticks total)
    double restVelocity = 0.05;       // below this the marginal band does not conduct
    double chatterP = 0.15;           // per-ms toggle probability inside the band while moving
    double phantomPerMsDriving = 0.0;
    double phantomPerMsIdle = 0.0;
    bool   invertedWiring = false;
    double backlashTicks = 0.0;       // play between shaft and head: head moves only once the shaft has crossed it
};

class RotaryModel {
  public:
    explicit RotaryModel(const RotaryParams& p = RotaryParams(), uint32_t seed = 2) : p_(p), rng_(seed) {}

    void setJammed(bool j) { jammed_ = j; }
    void setEncoderFrozen(bool f) { encoderFrozen_ = f; }
    void setDiagFor(int ms) { diagMs_ = ms; }
    void setSwitchStuckClosed(bool s) { stuck_ = s; }
    void setSwitchDisconnected(bool d) { disconnected_ = d; }
    void setSwitchPolarityWrong(bool w) { wrongPolarity_ = w; }
    void setPosition(double ticks) { pos_ = ticks; shaft_ = ticks; vel_ = 0; moving_ = false; dwell_ = 0; encoder_ = static_cast<Ticks>(std::floor(shaft_)) + phantom_; updateSwitch(); }
    double shaft() const { return shaft_; }
    void setParams(const RotaryParams& p) { p_ = p; }
    const RotaryParams& params() const { return p_; }

    double position() const { return pos_; }
    double velocity() const { return vel_; }
    bool   moving() const { return moving_; }
    Ticks  encoder() const { return encoder_; }
    bool   diag() const { return diagMs_ > 0; }
    bool   homeSwitch() const { return switch_; }
    uint32_t closures() const { return closures_; }      // raw open→closed transitions
    double travelled() const { return travelled_; }       // total |distance| moved, any direction
    // Distance in ticks from the cam centre, circular, always >= 0.
    double distToCentre() const {
        double m = std::fmod(pos_ - p_.camCentreTicks, p_.ticksPerRev);
        if (m < 0) m += p_.ticksPerRev;
        return m > p_.ticksPerRev / 2 ? p_.ticksPerRev - m : m;
    }

    void step(BridgeCommand cmd, bool driversEnabled) {
        bool driving = driversEnabled && cmd.mode == Bridge::Drive && cmd.duty != 0 && !jammed_;
        double d = driving ? std::fabs(cmd.duty) / 1000.0 : 0.0;
        int dir = driving ? (cmd.duty > 0 ? 1 : -1) : 0;
        if (p_.invertedWiring) dir = -dir;

        if (driving && d >= p_.breakawayDuty) ++dwell_; else dwell_ = 0;
        if (!moving_ && dwell_ >= p_.breakawayDwellMs) moving_ = true;

        if (moving_) {
            if (driving) {
                double eff = (d - p_.runningFriction) / (1.0 - p_.runningFriction);
                if (eff < 0) eff = 0;
                double vT = dir * p_.kv * eff;
                vel_ += (vT - vel_) * (1.0 / p_.tauDriveMs);
            } else if (!driversEnabled || cmd.mode == Bridge::Coast) {
                vel_ *= std::exp(-1.0 / p_.tauCoastMs);
            } else {
                vel_ *= std::exp(-1.0 / p_.tauBrakeMs);
            }
            if (!driving && std::fabs(vel_) < 0.002) { vel_ = 0; moving_ = false; }
        }
        if (jammed_) vel_ = 0;
        shaft_ += vel_;
        double half = p_.backlashTicks / 2.0;
        if (shaft_ - pos_ > half) pos_ = shaft_ - half;          // the head is dragged once the play is used up
        else if (shaft_ - pos_ < -half) pos_ = shaft_ + half;
        travelled_ += std::fabs(vel_);
        updateSwitch();

        if (!encoderFrozen_) {
            encoder_ = static_cast<Ticks>(std::floor(shaft_)) + phantom_;
            double pp = driving ? p_.phantomPerMsDriving : p_.phantomPerMsIdle;
            if (pp > 0 && rng_.chance(pp)) { phantom_ += rng_.chance(0.5) ? 1 : -1; encoder_ = static_cast<Ticks>(std::floor(shaft_)) + phantom_; }
        }
        if (diagMs_ > 0) --diagMs_;
    }

  private:
    void updateSwitch() {
        bool sw;
        double dist = distToCentre();
        bool fast = std::fabs(vel_) > p_.restVelocity;
        if (dist <= p_.halfWidthRest) { sw = true; inBand_ = false; }
        else if (dist <= p_.halfWidthMoving && fast) {
            if (!inBand_) { inBand_ = true; bandState_ = true; }          // first contact on entry
            else if (rng_.chance(p_.chatterP)) bandState_ = !bandState_;   // chatter
            sw = bandState_;
        } else { sw = false; inBand_ = false; }
        if (stuck_) sw = true;
        if (disconnected_) sw = false;
        if (wrongPolarity_) sw = !sw;
        if (sw && !switch_) ++closures_;
        switch_ = sw;
    }
    RotaryParams p_;
    Rng rng_;
    double pos_ = 0, shaft_ = 0, vel_ = 0, travelled_ = 0;
    bool moving_ = false, jammed_ = false, encoderFrozen_ = false;
    bool stuck_ = false, disconnected_ = false, wrongPolarity_ = false;
    bool switch_ = false, inBand_ = false, bandState_ = false;
    int dwell_ = 0;
    Ticks encoder_ = 0, phantom_ = 0;
    int diagMs_ = 0;
    uint32_t closures_ = 0;
};

}} // namespace uppity::sim
