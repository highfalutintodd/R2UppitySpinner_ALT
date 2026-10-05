#pragma once
#include <cstdint>
#include <cstring>
#include "Units.h"
#include "Parser.h"
#include "CommandQueue.h"
#include "StatusSnapshot.h"
#include "SequenceStore.h"
#include "TextSink.h"
#include "EventFormat.h"
// The non-blocking chain runner (docs/REWRITE-PLAN.md §3.4). One chain at a time. Each
// element is either a motion request handed to the single motor authority (the sequencer then
// waits for that program id to finish, or merely to start for a continuous spin), a timed wait,
// or an immediate action (lights, random mode, E-STOP). It never touches a motor itself and it
// never blocks loop(): tick() looks at the status snapshot and advances one step at most.
namespace uppity {

// Turn a parsed motion element into a request for the supervisor. Pure. travel = the current
// lifter travel in ticks (from the snapshot) so ":PP<pct>" becomes ticks here, in one place.
inline bool buildRequest(const Command& c, Ticks travel, RandomSource& rng, MotionRequest& r) {
    r = MotionRequest{};
    switch (c.verb) {
        case Verb::LiftTo: case Verb::LiftRandom: {
            int32_t pct = c.verb == Verb::LiftRandom ? rng.range(0, 100) : clampv<int32_t>(c.a, 0, 100);
            int32_t speed = c.hasB ? (c.b == -2 ? rng.range(30, 100) : clampv<int32_t>(c.b, 0, 100)) * 10 : 0;   // typed nonsense must not overflow
            r.kind = JobKind::LifterSeek; r.a = pct * travel / 100; r.b = speed; r.c = (pct >= 100 || pct <= 0) ? 1 : 0;
            return true;
        }
        case Verb::SafeLower: r.kind = JobKind::SafeLower; r.b = c.hasA ? clampv<int32_t>(c.a, 0, 100) * 10 : 0; return true;
        case Verb::RotAbs: case Verb::RotAbsRandom: case Verb::RotRel: case Verb::RotRelRandom: {
            bool rel = c.verb == Verb::RotRel || c.verb == Verb::RotRelRandom;
            bool rnd = c.verb == Verb::RotAbsRandom || c.verb == Verb::RotRelRandom;
            int32_t deg = rnd ? (rel ? rng.range(-180, 180) : rng.range(0, 359)) : (rel ? clampv<int32_t>(c.a, -36000, 36000) : c.a);   // 100 turns either way
            int32_t spd = c.hasB ? (c.b == -2 ? rng.range(40, 100) : clampv<int32_t>(c.b, 0, 100)) * 10 : 0;
            int32_t mx = c.hasC ? (c.c == -2 ? rng.range(40, 100) : clampv<int32_t>(c.c, 0, 100)) * 10 : 0;
            if (!rel && deg == 0) { r.kind = JobKind::RotaryHome; r.c = c.letter == 'S' ? 2 : 0; return true; }   // 'S': only if not already home (random director)
            r.kind = rel ? JobKind::RotaryRelative : JobKind::RotaryAbsolute; r.a = deg; r.b = spd; r.c = mx;
            return true;
        }
        case Verb::Spin:
            if (c.a == 0) { r.kind = JobKind::RotaryHome; return true; }
            r.kind = JobKind::RotarySpin; r.a = clampv<int32_t>(c.a, -100, 100) * 10; return true;
        case Verb::SpinRandom: {
            int32_t m = c.hasA ? clampv<int32_t>(c.a, 1, 100) : 80;
            int32_t v = rng.range(-m, m); if (v == 0) v = m;
            r.kind = JobKind::RotarySpin; r.a = v * 10; return true;
        }
        default: return false;
    }
}

class Sequencer {
  public:
    static constexpr int kMaxElements = 32;
    static constexpr Ms  kWaitClampMs = 60000;
    static constexpr int kMaxSplices = 8;

    // Immediate actions the chain can trigger; the console owns the real light kit and the
    // random director.
    struct Listener {
        virtual ~Listener() = default;
        virtual void onLight(int mode) = 0;             // 0..7, or -1 = random non-off
        virtual void onRandom(const Command& c) = 0;    // :PM / :PMG/M/A / :PM,a,b,c,d
        virtual void onRandomStop() = 0;                // a motion element from outside the director
        virtual void onEstop() = 0;                     // ":PX" met inside a stored sequence
        virtual void onRandomStalled() { onRandomStop(); }   // a random-mode move stalled: the show ends (D3)
    };

    void begin(CommandQueue& q, SnapshotPublisher& pub, SequenceStore& store, RandomSource& rng, TextSink& out, Listener& l) {
        q_ = &q; pub_ = &pub; store_ = &store; rng_ = &rng; out_ = &out; l_ = &l;
    }

    // Replace whatever is pending with this chain. A chain without motion elements runs its
    // immediate actions right now and leaves a running chain alone (":PL5" during a stored
    // sequence just changes the lights). A motion chain from outside the director ends random mode.
    bool submit(const Command* els, int n, ReqSource src, bool fromDirector) {
        if (n <= 0) return false;
        if (n > kMaxElements) n = kMaxElements;
        bool motion = false, timed = false;
        for (int i = 0; i < n; ++i) {
            if (isMotionVerb(els[i].verb) || els[i].verb == Verb::SpinStop) motion = true;
            if (els[i].verb == Verb::Wait) timed = true;
        }
        if (!motion && !timed) {
            for (int i = 0; i < n; ++i) immediate(els[i], src);
            return true;
        }
        if (motion && !fromDirector) l_->onRandomStop();
        if (n_ > 0) out_->printf("SEQ replaced at step %d/%d", idx_ + 1, n_);
        for (int i = 0; i < n; ++i) el_[i] = els[i];
        n_ = n; idx_ = 0; src_ = src; splices_ = 0; waitingId_ = 0; waiting_ = false; retries_ = 0;
        if (n_ > 1 || el_[0].verb == Verb::PlaySeq) out_->printf("SEQ start %d element%s", n_, n_ == 1 ? "" : "s");
        return true;
    }

    void clear(const char* why) {
        if (n_ > 0 && why) out_->printf("SEQ cleared at step %d/%d (%s)", idx_ + 1, n_, why);
        n_ = 0; idx_ = 0; waitingId_ = 0; waiting_ = false;
    }
    bool active() const { return n_ > 0; }
    ReqSource source() const { return src_; }
    uint32_t waitingFor() const { return waitingId_; }

    void tick(Ms now) {
        if (n_ == 0) return;
        Snapshot s; if (!pub_->read(s)) return;
        if (s.estop) { clear("ESTOP"); return; }
        if (waitingId_ != 0) {
            // a continuous spin is "done" for the chain once the spin step itself is running (a
            // ":PR50" from the bottom raises first; the wait after it must time the spin, not the raise)
            if (continuous_) { if ((s.activeJobId == waitingId_ && s.activeJob == JobKind::RotarySpin) || static_cast<int32_t>(s.lastProgramId - waitingId_) >= 0) { waitingId_ = 0; advance(); } }
            else if (static_cast<int32_t>(s.lastProgramId - waitingId_) >= 0) {
                if (s.lastProgramId == waitingId_ && s.lastResult == JobResult::Refused) { out_->printf("SEQ abort step %d/%d: %s REFUSED %s", idx_ + 1, n_, jobName(static_cast<int>(s.lastJob)), reasonName(s.lastReason)); n_ = 0; waitingId_ = 0; return; }
                // Random mode stops at the first stall or driver fault instead of driving into whatever
                // caught the mast or head every second or two (code review 2026-10, D3). A refusal is
                // not a stall: at power-up a descent is refused until the lifter is referenced.
                if (src_ == ReqSource::Random && s.lastProgramId == waitingId_ && (s.lastResult == JobResult::Stalled || s.lastResult == JobResult::FaultExit)) {
                    out_->printf("SEQ abort step %d/%d: %s %s %s", idx_ + 1, n_, jobName(static_cast<int>(s.lastJob)), s.lastResult == JobResult::Stalled ? "STALLED" : "FAULT", reasonName(s.lastReason));
                    n_ = 0; waitingId_ = 0; l_->onRandomStalled(); return;
                }
                waitingId_ = 0; advance();
            }
            return;
        }
        if (waiting_) { if (elapsed(now, waitStart_) >= waitMs_) { waiting_ = false; advance(); } return; }
        if (idx_ >= n_) { n_ = 0; return; }
        const Command& c = el_[idx_];
        switch (c.verb) {
            case Verb::Wait: {
                Ms ms;
                if (c.b == 2) ms = static_cast<Ms>(c.a);                                                   // internal, milliseconds
                else {
                    int32_t sec = c.hasA ? c.a : 6; if (c.b == 1) sec = rng_->range(1, sec < 1 ? 1 : sec); if (sec < 1) sec = 1;
                    // compared in seconds: ":PW4294968" multiplied to milliseconds wrapped to 0.7 s
                    if (static_cast<Ms>(sec) > kWaitClampMs / 1000u) { out_->printf("WAIT clamped from %lds to 60s", static_cast<long>(sec)); sec = static_cast<int32_t>(kWaitClampMs / 1000u); }
                    ms = static_cast<Ms>(sec) * 1000u;
                }
                if (ms > kWaitClampMs) ms = kWaitClampMs;
                waiting_ = true; waitStart_ = now; waitMs_ = ms;
                return;
            }
            case Verb::PlaySeq: {
                if (++splices_ > kMaxSplices) { out_->printf("SEQ abort: :PS nested too deep"); n_ = 0; return; }
                char text[SequenceStore::kMaxLen + 1];
                if (!store_->read(clampv<int32_t>(c.a, 0, SequenceStore::kCount - 1), text, sizeof text)) { out_->printf("SEQ %ld: not stored", static_cast<long>(c.a)); advance(); return; }
                Command tmp[kMaxElements]; int m = parseChain(text, tmp, kMaxElements, nullptr);
                // #PS only stores what parses; a slot that does not (flash damage, or a later grammar)
                // is skipped, never spliced with a negative count
                if (m < 0) { out_->printf("SEQ %ld: unreadable, skipped", static_cast<long>(c.a)); advance(); return; }
                splice(idx_, tmp, m);
                out_->printf("SEQ %ld: %d element%s", static_cast<long>(c.a), m, m == 1 ? "" : "s");
                return;                                                     // the spliced elements start at idx_
            }
            case Verb::Light: case Verb::LightRandom: case Verb::Random: case Verb::RandomLevel:
                immediate(c, src_); advance(); return;
            case Verb::Estop: l_->onEstop(); n_ = 0; return;
            case Verb::SpinStop: q_->cancel(); advance(); return;
            default: {
                MotionRequest r;
                if (!buildRequest(c, s.lifterTravel, *rng_, r)) { advance(); return; }
                r.source = src_;
                uint32_t id = q_->push(r);
                if (id == 0) { if (++retries_ > 2000) { out_->printf("SEQ abort: queue full"); n_ = 0; } return; }   // retry next tick
                retries_ = 0;
                continuous_ = r.kind == JobKind::RotarySpin;
                waitingId_ = id;
                if (n_ > 1) out_->printf("QUEUED=%s id=%lu (step %d/%d)", jobName(static_cast<int>(r.kind)), static_cast<unsigned long>(id), idx_ + 1, n_);
                else out_->printf("QUEUED=%s id=%lu", jobName(static_cast<int>(r.kind)), static_cast<unsigned long>(id));
                return;
            }
        }
    }

    // Split a ":P..:P.." line (or a stored body) into parsed elements. Returns the count, or
    // -1 with the bad element in `bad` (may be null) when one does not parse.
    static int parseChain(const char* line, Command* out, int cap, char* bad, size_t badN = 0) {
        const char* cursor = line; char el[64], norm[72]; int n = 0;
        bool first = true;
        while (nextChainElement(cursor, el, sizeof el)) {
            const char* text = el;
            if (first) { first = false; if (el[0] != ':' && el[0] != '#') { normaliseElement(el, norm, sizeof norm); text = norm; } }
            else { if (!normaliseElement(el, norm, sizeof norm)) continue; text = norm; }
            Command c;
            if (!parseLine(text, c) || c.verb == Verb::Invalid || !isChainVerb(c.verb)) { if (bad && badN) std::snprintf(bad, badN, "%s", el); return -1; }
            if (n < cap) out[n++] = c;
        }
        return n;
    }

  private:
    void immediate(const Command& c, ReqSource src) {
        switch (c.verb) {
            case Verb::Light: l_->onLight(clampv<int32_t>(c.a, 0, 7)); break;
            case Verb::LightRandom: l_->onLight(-1); break;
            case Verb::Random: case Verb::RandomLevel:
                if (!randomAllowedFrom(src)) { out_->printf("RANDOM=refused (commissioning in progress: only the page or the USB console starts it)"); break; }
                l_->onRandom(c); break;
            case Verb::Estop: l_->onEstop(); break;
            default: break;
        }
    }
    // While commissioning has lifted the lock, random mode's moves are let through (step 7 runs it),
    // so random mode itself may only be started by someone watching: a ":PM" from the dome's bus
    // would move a board whose direction and travel are not yet known (code review 2026-10).
    bool randomAllowedFrom(ReqSource src) const {
        Snapshot s; if (!pub_->read(s)) return false;
        if (!(s.commissioning && s.state == MotionState::Locked)) return true;
        return src == ReqSource::SerialUsb || src == ReqSource::Web || src == ReqSource::Commission || src == ReqSource::Test;
    }
    void advance() {
        ++idx_;
        if (idx_ >= n_) { if (n_ > 1) out_->printf("SEQ done"); n_ = 0; idx_ = 0; }
    }
    void splice(int at, const Command* els, int m) {
        // replace el_[at] with els[0..m)
        int tail = n_ - at - 1;
        int room = kMaxElements - (n_ - 1);
        if (m > room) m = room;
        if (m != 1) std::memmove(&el_[at + m], &el_[at + 1], static_cast<size_t>(tail) * sizeof(Command));
        for (int i = 0; i < m; ++i) el_[at + i] = els[i];
        n_ = n_ - 1 + m;
        if (m == 0 && idx_ >= n_) n_ = 0;
    }

    CommandQueue* q_ = nullptr; SnapshotPublisher* pub_ = nullptr; SequenceStore* store_ = nullptr;
    RandomSource* rng_ = nullptr; TextSink* out_ = nullptr; Listener* l_ = nullptr;
    Command el_[kMaxElements];
    int n_ = 0, idx_ = 0, splices_ = 0, retries_ = 0;
    ReqSource src_ = ReqSource::None;
    uint32_t waitingId_ = 0; bool continuous_ = false;
    bool waiting_ = false; Ms waitStart_ = 0, waitMs_ = 0;
};

} // namespace uppity
