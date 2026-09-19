#pragma once
#include <cstdint>
#include "Units.h"
// The hardware abstraction the pure core drives. Two implementations: Esp32Hal (glue) and
// SimHal (tests). Everything the supervisor tick calls must be safe at 1 kHz: register and
// GPIO access only, no I2C, no blocking.
namespace uppity {

enum class Axis : uint8_t { Lifter = 0, Rotary = 1 };
enum class SwitchId : uint8_t { LifterTop = 0, LifterBottom = 1, RotaryHome = 2 };

// TB9051FTG output states. Drive uses the sign of duty for direction.
enum class Bridge : uint8_t { Coast, Brake, Drive };

struct BridgeCommand {
    Bridge   mode = Bridge::Coast;
    Throttle duty = 0;             // only meaningful for Drive
    bool operator==(const BridgeCommand& o) const { return mode == o.mode && duty == o.duty; }
    bool operator!=(const BridgeCommand& o) const { return !(*this == o); }
};

struct RawInputs {
    Ticks lifterTicks = 0;
    Ticks rotaryTicks = 0;
    bool  topLimit = false;        // polarity already applied: true = switch made
    bool  bottomLimit = false;
    bool  homeSwitch = false;
    bool  lifterDiag = false;      // true = driver reports a fault
    bool  rotaryDiag = false;
    bool  enableAcked = false;     // expander confirms drivers are enabled
};

class Hal {
  public:
    virtual ~Hal() = default;
    virtual Ms       nowMs() const = 0;
    virtual uint32_t nowUs() const = 0;
    virtual void readInputs(RawInputs& out) = 0;                // tick-safe
    virtual void writeBridge(Axis a, BridgeCommand c) = 0;      // tick-safe, LEDC only
    virtual void panicStop() = 0;                               // any context: all PWM to zero
    virtual void requestEnable(bool on) = 0;                    // sets an atomic; I2C service acts
    virtual void setEncoder(Axis a, Ticks ticks) = 0;           // re-reference the count
    virtual void setLifterInverted(bool inverted) = 0;          // swap drive sense and count sense together
    // Limit/home switch wiring: activeLow = true means a made switch reads LOW (switch to ground,
    // pull-up on the pin), the normally-open default. Applied before RawInputs is filled.
    virtual void setSwitchActiveLow(SwitchId which, bool activeLow) = 0;
};

// Mutual exclusion for the command queue. portMUX on the ESP32, a no-op on the host.
struct Lock {
    virtual ~Lock() = default;
    virtual void lock() = 0;
    virtual void unlock() = 0;
};
struct NoLock final : Lock {
    void lock() override {}
    void unlock() override {}
};

} // namespace uppity
