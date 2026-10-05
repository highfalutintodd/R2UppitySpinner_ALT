#pragma once
#include <cstdint>
#include <cstring>
#include "Units.h"
#include "Parser.h"
#include "Sequencer.h"
#include "TextSink.h"
// Per-line acceptance and routing (docs/REWRITE-PLAN.md §3.4 / A.3): the Marcduino id filter,
// the cross-transport duplicate filter, then "#P" lines to the configuration handler and ":P"
// chains to the sequencer. Every serial source feeds its own LineAssembler; they meet here.
namespace uppity {

// Identical lines from different transports inside the window run once (a Sabé button that
// arrives over both the mesh and the serial bus). The same transport repeating a line is
// deliberate and always runs.
class LineDedup {
  public:
    bool accept(const char* line, ReqSource src, Ms now, Ms windowMs) {
        uint32_t h = 2166136261u;
        for (const char* p = line; *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
        bool dup = windowMs > 0 && lastHash_ == h && lastSrc_ != src && lastSrc_ != ReqSource::None && elapsed(now, lastAt_) < windowMs;
        if (!dup) { lastHash_ = h; lastSrc_ = src; lastAt_ = now; }
        return !dup;
    }
  private:
    uint32_t lastHash_ = 0; ReqSource lastSrc_ = ReqSource::None; Ms lastAt_ = 0;
};

// The three expander inputs (A/B/C, grounded = active) as a 3-bit value; a debounced change to
// a non-zero value plays stored sequence <value>. Sampled every ~20 ms by the I2C service.
class TriggerInputs {
  public:
    static constexpr int kStableSamples = 3;
    // Returns 1..7 once, when a new non-zero value has been stable; 0 otherwise. The first
    // stable reading after boot only sets the baseline: a jumper left in place must never
    // play a sequence at power-up (v3 fired on changes only, too).
    int feed(uint8_t value) {
        value &= 7;
        if (value == candidate_) { if (count_ < 255) ++count_; } else { candidate_ = value; count_ = 1; }
        if (count_ != kStableSamples) return 0;
        if (!primed_) { primed_ = true; stable_ = candidate_; return 0; }
        if (candidate_ != stable_) { stable_ = candidate_; return stable_; }
        return 0;
    }
    uint8_t stable() const { return stable_; }
    bool primed() const { return primed_; }
  private:
    uint8_t candidate_ = 0, stable_ = 0, count_ = 0;
    bool primed_ = false;
};

class Dispatcher {
  public:
    struct ConfigHandler {
        virtual ~ConfigHandler() = default;
        virtual void onConfig(const Command& c, const char* line, ReqSource src) = 0;
    };

    void begin(Sequencer& seq, ConfigHandler& h, TextSink& out) { seq_ = &seq; h_ = &h; out_ = &out; }
    void setId(uint8_t id) { id_ = id; }
    void setDedupMs(uint16_t ms) { dedupMs_ = ms; }
    uint32_t droppedForId() const { return droppedId_; }
    uint32_t droppedDuplicates() const { return droppedDup_; }

    // A complete ':' or '#' line from one source. Returns false when the line was dropped.
    bool handleLine(const char* line, ReqSource src, Ms now) {
        if (!dedup_.accept(line, src, now, dedupMs_)) { ++droppedDup_; return false; }
        if (line[0] == '#') {
            Command c;
            if (!parseLine(line, c)) return false;
            if (c.id >= 0 && c.id != id_) { ++droppedId_; return false; }
            h_->onConfig(c, line, src);
            return true;
        }
        Command els[Sequencer::kMaxElements];
        char bad[64];
        // A ':' line that is not ":P..." is another board's (":SE00" for the dome panels, ":OP01"...):
        // silent, never INVALID (hard rule 7; v3 was silent too). The id, if any, is on the first
        // element; a mismatch drops the whole line silently.
        { const char* p = line; while (*p == ' ') ++p; if (p[1] != 'P') return false; }
        { Command first; if (parseLine(line, first) && first.id >= 0 && first.id != id_) { ++droppedId_; return false; } }
        int n = Sequencer::parseChain(line, els, Sequencer::kMaxElements, bad, sizeof bad);
        if (n < 0) { out_->printf("INVALID=%s", bad); return false; }
        if (n == 0) return false;
        return seq_->submit(els, n, src, false);
    }

    // The random director's chains come straight in (they may not end random mode).
    bool submitDirector(const Command* els, int n) { return seq_->submit(els, n, ReqSource::Random, true); }

  private:
    Sequencer* seq_ = nullptr; ConfigHandler* h_ = nullptr; TextSink* out_ = nullptr;
    LineDedup dedup_;
    uint8_t id_ = 0; uint16_t dedupMs_ = 750;
    uint32_t droppedId_ = 0, droppedDup_ = 0;
};

} // namespace uppity
