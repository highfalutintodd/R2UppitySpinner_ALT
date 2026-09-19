#pragma once
#ifdef ARDUINO
#include <atomic>
#include "../core/Hal.h"
// The real board behind the Hal interface: LEDC PWM at 10 kHz, PCNT hardware pulse counters
// with the glitch filter, direct GPIO for limits and DIAG, and an enable request that the
// I2C service in loop() applies to the PCF8574 (never from the supervisor tick).
namespace uppity {

class Esp32Hal final : public Hal {
  public:
    bool begin();                                   // call once from setup(); false = PCNT/LEDC setup failed

    Ms       nowMs() const override;
    uint32_t nowUs() const override;
    void readInputs(RawInputs& out) override;
    void writeBridge(Axis a, BridgeCommand c) override;
    void panicStop() override;
    void requestEnable(bool on) override;
    void setEncoder(Axis a, Ticks ticks) override;
    void setLifterInverted(bool inverted) override;
    void setSwitchActiveLow(SwitchId which, bool activeLow) override;

    // ---- for the I2C service (loop context) ----
    bool enableRequested() const { return enableRequested_.load(std::memory_order_acquire); }
    void setEnableAcked(bool acked) { enableAcked_.store(acked, std::memory_order_release); }
    const char* lastError() const { return lastError_; }

  private:
    struct Counter { void* unit = nullptr; Ticks offset = 0; };
    bool makeCounter(Counter& c, int pinA, int pinB);
    Ticks readCounter(const Counter& c) const;
    Counter lifterCnt_, rotaryCnt_;
    BridgeCommand last_[2];
    bool lifterInverted_ = false;
    bool activeLow_[3] = { true, true, true };
    std::atomic<bool> enableRequested_{false};
    std::atomic<bool> enableAcked_{false};
    const char* lastError_ = "";
};

} // namespace uppity
#endif
