#pragma once
#include <atomic>
#include <cstdint>
#include "Units.h"
#include "Hal.h"
#include "StatusSnapshot.h"
// Requests from every source (serial, web, mesh, sequencer, random director) to the single
// motion authority. Multi-producer under a Lock, single consumer (the supervisor tick).
// E-STOP never travels through here; it is a latch with its own path.
namespace uppity {

enum class ReqSource : uint8_t { None = 0, SerialUsb, SerialCmd, Web, Mesh, Sequencer, Random, Trigger, Commission, Test };

struct MotionRequest {
    JobKind   kind = JobKind::None;
    int32_t   a = 0;          // meaning per kind: target ticks / deg*10 / throttle ...
    int32_t   b = 0;          // speed (0.1 %) / max speed ...
    int32_t   c = 0;
    ReqSource source = ReqSource::None;
    uint32_t  id = 0;         // assigned by the queue; echoed in JobStart/JobEnd events
};

class CommandQueue {
  public:
    static constexpr int kDepth = 8;
    explicit CommandQueue(Lock& lock) : lock_(lock) {}

    // Returns the request id, or 0 if the queue is full (caller reports Busy).
    uint32_t push(MotionRequest r) {
        lock_.lock();
        uint32_t id = 0;
        if (count_ < kDepth) {
            id = ++nextId_;
            r.id = id;
            buf_[(head_ + count_) % kDepth] = r;
            ++count_;
        }
        lock_.unlock();
        return id;
    }
    bool pop(MotionRequest& out) {
        lock_.lock();
        bool ok = count_ > 0;
        if (ok) { out = buf_[head_]; head_ = (head_ + 1) % kDepth; --count_; }
        lock_.unlock();
        return ok;
    }
    void flush() { lock_.lock(); head_ = 0; count_ = 0; lock_.unlock(); }
    int  size() { lock_.lock(); int n = count_; lock_.unlock(); return n; }

    // Out-of-band controls (atomics, any context).
    void cancel() { cancel_.store(true, std::memory_order_release); }
    bool takeCancel() { return cancel_.exchange(false, std::memory_order_acq_rel); }
    void jogKeepalive(Ms nowMs) { jogUntil_.store(nowMs + kJogLeaseMs, std::memory_order_release); }
    // The page's finger came off the jog button: end the lease now instead of letting it run out.
    // (Stored as "now", not 0: 0 would read as a live lease again once millis() passes 2^31.)
    void jogRelease(Ms nowMs) { jogUntil_.store(nowMs, std::memory_order_release); }
    bool jogLeaseLive(Ms nowMs) const { return static_cast<int32_t>(jogUntil_.load(std::memory_order_acquire) - nowMs) > 0; }
    void setOverride(Ms untilMs) { overrideUntil_.store(untilMs, std::memory_order_release); }
    Ms   overrideUntil() const { return overrideUntil_.load(std::memory_order_acquire); }
    bool overrideLive(Ms nowMs) const { Ms u = overrideUntil(); return u != 0 && static_cast<int32_t>(u - nowMs) > 0; }

    static constexpr Ms kJogLeaseMs = 2000;
    static constexpr Ms kOverrideMs = 60000;

  private:
    Lock& lock_;
    MotionRequest buf_[kDepth];
    int head_ = 0, count_ = 0;
    uint32_t nextId_ = 0;
    std::atomic<bool> cancel_{false};
    std::atomic<uint32_t> jogUntil_{0};
    std::atomic<uint32_t> overrideUntil_{0};
};

} // namespace uppity
