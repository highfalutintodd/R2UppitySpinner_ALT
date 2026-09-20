#pragma once
#include <cstdint>
#include "../core/Hal.h"
#include "LifterModel.h"
#include "RotaryModel.h"
// The simulated board: both mechanism models, the expander's enable latency, a fake clock, and
// a record of every bridge command so tests can assert "PWM zero within N ms of the trip" and
// "no drive after the latch".
namespace uppity { namespace sim {

class SimHal final : public Hal {
  public:
    LifterModel lifter;
    RotaryModel rotary;

    int  enableLatencyMs = 5;      // I2C service turnaround for EN/ENB
    bool failEnable = false;       // expander fault injection
    bool forceTop = false;         // a fingertip on the top limit switch (bench step 4)
    bool forceBottom = false;

    // ---- Hal ----
    Ms       nowMs() const override { return nowMs_; }
    uint32_t nowUs() const override { return nowMs_ * 1000u; }
    void readInputs(RawInputs& out) override {
        out.lifterTicks = lifterSign_ * lifter.encoder() + lifterOffset_;
        out.rotaryTicks = rotary.encoder() + rotaryOffset_;
        // Models report "made"; the wiring polarity is what the HAL would see and correct for.
        out.topLimit    = lifter.topLimit() || forceTop;
        out.bottomLimit = lifter.bottomLimit() || forceBottom;
        out.homeSwitch  = rotary.homeSwitch();
        out.lifterDiag  = lifter.diag();
        out.rotaryDiag  = rotary.diag();
        out.enableAcked = enableAcked_;
        ++inputReads_;
    }
    void writeBridge(Axis a, BridgeCommand c) override {
        int i = static_cast<int>(a);
        if (a == Axis::Lifter && lifterSign_ < 0 && c.mode == Bridge::Drive) c.duty = static_cast<Throttle>(-c.duty);
        if (cmd_[i] != c) { cmd_[i] = c; record(a, c); }
        if (c.mode == Bridge::Drive && c.duty != 0) { ++driveWrites_; lastDriveAt_ = nowMs_; }
    }
    void panicStop() override {
        ++panicCount_;
        BridgeCommand b; b.mode = Bridge::Brake; b.duty = 0;
        for (int i = 0; i < 2; ++i) { if (cmd_[i] != b) { cmd_[i] = b; record(static_cast<Axis>(i), b); } }
        lastPanicAt_ = nowMs_;
    }
    void requestEnable(bool on) override {
        if (on != enableRequested_) { enableRequested_ = on; enableChangeAt_ = nowMs_; ++enableWrites_; }
    }
    void setEncoder(Axis a, Ticks ticks) override {
        if (a == Axis::Lifter) lifterOffset_ = ticks - lifterSign_ * lifter.encoder();
        else rotaryOffset_ = ticks - rotary.encoder();
    }
    void setLifterInverted(bool inv) override { lifterSign_ = inv ? -1 : 1; }
    void setSwitchActiveLow(SwitchId, bool) override { /* models report "made" directly */ }

    // ---- simulation control ----
    void advance(int ms) {
        for (int k = 0; k < ms; ++k) {
            lifter.step(cmd_[0], enableAcked_);
            rotary.step(cmd_[1], enableAcked_);
            ++nowMs_;
            if (failEnable) enableAcked_ = false;
            else if (enableAcked_ != enableRequested_ && static_cast<int32_t>(nowMs_ - enableChangeAt_) >= enableLatencyMs) enableAcked_ = enableRequested_;
        }
    }
    BridgeCommand command(Axis a) const { return cmd_[static_cast<int>(a)]; }
    bool enableRequested() const { return enableRequested_; }
    bool enableAcked() const { return enableAcked_; }
    uint32_t driveWrites() const { return driveWrites_; }
    uint32_t enableWrites() const { return enableWrites_; }
    uint32_t inputReads() const { return inputReads_; }
    uint32_t panicCount() const { return panicCount_; }
    Ms lastDriveAt() const { return lastDriveAt_; }
    Ms lastPanicAt() const { return lastPanicAt_; }

    struct Rec { Ms t; Axis a; BridgeCommand c; };
    static constexpr int kRecs = 256;
    int recordCount() const { return recCount_; }
    const Rec& rec(int i) const { return recs_[(recHead_ + kRecs - recCount_ + i) % kRecs]; } // oldest first
    void clearRecords() { recCount_ = 0; }

  private:
    void record(Axis a, BridgeCommand c) {
        recs_[recHead_] = Rec{nowMs_, a, c};
        recHead_ = (recHead_ + 1) % kRecs;
        if (recCount_ < kRecs) ++recCount_;
    }
    Ms nowMs_ = 0;
    BridgeCommand cmd_[2];
    bool enableRequested_ = false, enableAcked_ = false;
    Ms enableChangeAt_ = 0;
    Ticks lifterOffset_ = 0, rotaryOffset_ = 0;
    int lifterSign_ = 1;
    uint32_t driveWrites_ = 0, enableWrites_ = 0, inputReads_ = 0, panicCount_ = 0;
    Ms lastDriveAt_ = 0, lastPanicAt_ = 0;
    Rec recs_[kRecs] = {};
    int recHead_ = 0, recCount_ = 0;
};

}} // namespace uppity::sim
