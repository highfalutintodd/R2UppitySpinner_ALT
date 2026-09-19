#ifdef ARDUINO
#include <Arduino.h>
#include "driver/pulse_cnt.h"
#include "Esp32Hal.h"
#include "../../Board.h"

namespace uppity {

namespace {
constexpr int kPwmMax = (1 << board::kPwmBits) - 1;
inline int pinPwm1(Axis a) { return a == Axis::Lifter ? board::kLifterPwm1 : board::kRotaryPwm1; }
inline int pinPwm2(Axis a) { return a == Axis::Lifter ? board::kLifterPwm2 : board::kRotaryPwm2; }
}

bool Esp32Hal::makeCounter(Counter& c, int pinA, int pinB) {
    pcnt_unit_config_t uc = {};
    uc.low_limit = -32768;
    uc.high_limit = 32767;
    uc.flags.accum_count = 1;                       // driver accumulates across the 16-bit limits
    pcnt_unit_handle_t unit = nullptr;
    if (pcnt_new_unit(&uc, &unit) != ESP_OK) { lastError_ = "pcnt_new_unit"; return false; }
    pcnt_glitch_filter_config_t fc = {};
    fc.max_glitch_ns = 10000;                       // 10 us: encoder edges are >= 200 us apart at full speed
    if (pcnt_unit_set_glitch_filter(unit, &fc) != ESP_OK) { lastError_ = "pcnt glitch filter"; return false; }
    pcnt_chan_config_t cc = {};
    cc.edge_gpio_num = pinA;
    cc.level_gpio_num = pinB;
    pcnt_channel_handle_t chan = nullptr;
    if (pcnt_new_channel(unit, &cc, &chan) != ESP_OK) { lastError_ = "pcnt_new_channel"; return false; }
    // x1 decode, as v3 counted: count the positive edge of A, direction from the level of B.
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_unit_add_watch_point(unit, 32767);
    pcnt_unit_add_watch_point(unit, -32768);
    if (pcnt_unit_enable(unit) != ESP_OK) { lastError_ = "pcnt_unit_enable"; return false; }
    pcnt_unit_clear_count(unit);
    pcnt_unit_start(unit);
    c.unit = unit;
    c.offset = 0;
    return true;
}

Ticks Esp32Hal::readCounter(const Counter& c) const {
    int v = 0;
    if (c.unit) pcnt_unit_get_count(static_cast<pcnt_unit_handle_t>(c.unit), &v);
    return static_cast<Ticks>(v) + c.offset;
}

bool Esp32Hal::begin() {
    const int pwmPins[4] = { board::kLifterPwm1, board::kLifterPwm2, board::kRotaryPwm1, board::kRotaryPwm2 };
    for (int p : pwmPins) {
        if (!ledcAttach(p, board::kPwmHz, board::kPwmBits)) { lastError_ = "ledcAttach"; return false; }
        ledcWrite(p, 0);
    }
    pinMode(board::kLifterTop, INPUT_PULLUP);
    pinMode(board::kLifterBottom, INPUT_PULLUP);
    pinMode(board::kRotaryHome, INPUT_PULLUP);
    pinMode(board::kLifterDiag, INPUT);              // 36/39 are input-only, no internal pull-ups (board provides them)
    pinMode(board::kRotaryDiag, INPUT);
    if (!makeCounter(lifterCnt_, board::kLifterEncA, board::kLifterEncB)) return false;
    if (!makeCounter(rotaryCnt_, board::kRotaryEncA, board::kRotaryEncB)) return false;
    return true;
}

Ms Esp32Hal::nowMs() const { return millis(); }
uint32_t Esp32Hal::nowUs() const { return micros(); }

void Esp32Hal::readInputs(RawInputs& out) {
    Ticks l = readCounter(lifterCnt_);
    out.lifterTicks = lifterInverted_ ? -l : l;
    out.rotaryTicks = readCounter(rotaryCnt_);
    auto made = [&](int pin, SwitchId id) {
        bool low = digitalRead(pin) == LOW;
        return activeLow_[static_cast<int>(id)] ? low : !low;
    };
    out.topLimit    = made(board::kLifterTop, SwitchId::LifterTop);
    out.bottomLimit = made(board::kLifterBottom, SwitchId::LifterBottom);
    out.homeSwitch  = made(board::kRotaryHome, SwitchId::RotaryHome);
    out.lifterDiag  = digitalRead(board::kLifterDiag) == LOW;
    out.rotaryDiag  = digitalRead(board::kRotaryDiag) == LOW;
    out.enableAcked = enableAcked_.load(std::memory_order_acquire);
}

void Esp32Hal::writeBridge(Axis a, BridgeCommand c) {
    if (a == Axis::Lifter && lifterInverted_ && c.mode == Bridge::Drive) c.duty = static_cast<Throttle>(-c.duty);
    int i = static_cast<int>(a);
    if (last_[i] == c) return;                        // LEDC writes only on change
    int p1 = pinPwm1(a), p2 = pinPwm2(a);
    switch (c.mode) {
        case Bridge::Drive: {
            // Datasheet 7.1 Note 1: forward<->reverse must pass through short brake or the IC
            // may be damaged. The supervisor already inserts a brake tick on reversal; this is
            // the belt to that brace, at the cost of 20 us inside the tick.
            bool reversing = last_[i].mode == Bridge::Drive && last_[i].duty != 0 &&
                             ((last_[i].duty < 0) != (c.duty < 0)) && c.duty != 0;
            if (reversing) { ledcWrite(p1, 0); ledcWrite(p2, 0); delayMicroseconds(20); }
            // Datasheet Table 10.1: both PWM halves must be >= 10 us. Keep pulse trains inside
            // the 12-88 % band; 0 is brake, 1000 is a steady level (DC), both legal.
            int permille = static_cast<int>(c.duty < 0 ? -c.duty : c.duty);
            if (permille > 0 && permille < board::kPwmMinDutyPermille) permille = board::kPwmMinDutyPermille;
            if (permille > board::kPwmMaxDutyPermille && permille < kThrottleMax) permille = board::kPwmMaxDutyPermille;
            int duty = (permille * kPwmMax) / kThrottleMax;                 // 1000 -> 1023 = full on
            if (c.duty >= 0) { ledcWrite(p2, 0); ledcWrite(p1, duty); }
            else             { ledcWrite(p1, 0); ledcWrite(p2, duty); }
            break;
        }
        case Bridge::Brake:                           // TB9051FTG Table 7.1-1: PWM1=PWM2=L is "short brake"
            ledcWrite(p1, 0); ledcWrite(p2, 0);       // (both low-side FETs on; H/H is the same state)
            break;
        case Bridge::Coast:                           // true High-Z needs EN=L or ENB=H (I2C); brake meanwhile
            ledcWrite(p1, 0); ledcWrite(p2, 0);
            break;
    }
    last_[i] = c;
}

void Esp32Hal::panicStop() {
    ledcWrite(board::kLifterPwm1, 0); ledcWrite(board::kLifterPwm2, 0);
    ledcWrite(board::kRotaryPwm1, 0); ledcWrite(board::kRotaryPwm2, 0);
    BridgeCommand b; b.mode = Bridge::Brake;
    last_[0] = b; last_[1] = b;
}

void Esp32Hal::requestEnable(bool on) { enableRequested_.store(on, std::memory_order_release); }

void Esp32Hal::setEncoder(Axis a, Ticks ticks) {
    Counter& c = a == Axis::Lifter ? lifterCnt_ : rotaryCnt_;
    int raw = 0;
    if (c.unit) pcnt_unit_get_count(static_cast<pcnt_unit_handle_t>(c.unit), &raw);
    Ticks want = (a == Axis::Lifter && lifterInverted_) ? -ticks : ticks;
    c.offset = want - static_cast<Ticks>(raw);       // no clear: no race with the hardware counter
}

void Esp32Hal::setLifterInverted(bool inverted) { lifterInverted_ = inverted; }
void Esp32Hal::setSwitchActiveLow(SwitchId which, bool activeLow) { activeLow_[static_cast<int>(which)] = activeLow; }

} // namespace uppity
#endif
