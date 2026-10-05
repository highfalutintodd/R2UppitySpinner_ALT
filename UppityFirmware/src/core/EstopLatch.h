#pragma once
#include <atomic>
#include <cstdint>
#include "Units.h"
// One-way emergency-stop latch. trip() is legal from ANY context (UART event task, WiFi task,
// web task, supervisor) and never blocks: it sets an atomic and calls the panic hook, which
// zeroes every PWM output directly. Only a deliberate release() clears it.
namespace uppity {

class EstopLatch {
  public:
    enum class Source : uint8_t { None = 0, SerialUsb, SerialCmd, Web, Mesh, Fault, Watchdog, Test, Sequence };
    using PanicFn = void (*)(void* ctx);

    void setPanic(PanicFn fn, void* ctx) { panicFn_ = fn; panicCtx_ = ctx; }

    // Idempotent. The first trip records the source and time; later trips only bump the count.
    // The flag goes up before the panic hook runs, so a supervisor tick that re-reads it just
    // before its PWM writes sees it once the outputs have been zeroed.
    void trip(Source s, Ms nowMs) {
        bool was = tripped_.exchange(true, std::memory_order_acq_rel);
        if (panicFn_) panicFn_(panicCtx_);
        if (!was) {
            source_.store(static_cast<uint8_t>(s), std::memory_order_relaxed);
            at_.store(nowMs, std::memory_order_relaxed);
            seq_.fetch_add(1, std::memory_order_acq_rel);
        }
        trips_.fetch_add(1, std::memory_order_relaxed);
    }
    bool tripped() const { return tripped_.load(std::memory_order_acquire); }

    // Deliberate only: web Reset, serial #PRESUME, mesh &ESTOP,RESET. Returns false if not tripped.
    bool release(Ms nowMs) {
        bool was = tripped_.exchange(false, std::memory_order_acq_rel);
        if (was) { releasedAt_.store(nowMs, std::memory_order_relaxed); releases_.fetch_add(1, std::memory_order_relaxed); }
        return was;
    }

    Source   source() const { return static_cast<Source>(source_.load(std::memory_order_relaxed)); }
    Ms       trippedAt() const { return at_.load(std::memory_order_relaxed); }
    uint32_t seq() const { return seq_.load(std::memory_order_acquire); }
    uint32_t tripCount() const { return trips_.load(std::memory_order_relaxed); }
    uint32_t releaseCount() const { return releases_.load(std::memory_order_relaxed); }

    static const char* sourceName(Source s) {
        switch (s) {
            case Source::SerialUsb: return "usb";
            case Source::SerialCmd: return "serial";
            case Source::Web:       return "web";
            case Source::Mesh:      return "mesh";
            case Source::Fault:     return "fault";
            case Source::Watchdog:  return "watchdog";
            case Source::Test:      return "test";
            case Source::Sequence:  return "sequence";
            default:                return "none";
        }
    }

  private:
    std::atomic<bool>     tripped_{false};
    std::atomic<uint8_t>  source_{0};
    std::atomic<uint32_t> at_{0};
    std::atomic<uint32_t> releasedAt_{0};
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> trips_{0};
    std::atomic<uint32_t> releases_{0};
    PanicFn panicFn_ = nullptr;
    void*   panicCtx_ = nullptr;
};

} // namespace uppity
