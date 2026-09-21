#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include "../core/StatusSnapshot.h"
#include "../../Board.h"
// The one NeoPixel (docs/REWRITE-PLAN.md §3.8): LOCKED solid yellow, IDLE home solid green,
// IDLE away solid red, MOVING green/yellow, STOPPED (E-STOP latched) fast red blink, FAULT
// red/yellow. Random mode adds a slow blue pulse on the idle colour. Dim, as v3 (max 6/255).
namespace uppity {
class StatusLed {
  public:
    void begin() { rgbLedWrite(board::kStatusLed, 0, 0, 0); }
    void update(const Snapshot& s, bool randomMode, bool wifiClient, uint32_t now) {
        if (now - last_ < 100) return;
        last_ = now;
        bool blink = (now / 250) & 1;
        uint8_t r = 0, g = 0, b = 0;
        switch (s.state) {
            case MotionState::Locked:  r = 2; g = 2; break;
            case MotionState::Stopped: r = blink ? 6 : 0; break;
            case MotionState::Fault:   if (blink) r = 4; else { r = 2; g = 2; } break;
            case MotionState::Moving:  if (blink) g = 3; else { r = 2; g = 2; } break;
            default: {
                bool home = s.homeConf == HomeConfidence::Latched || s.homeConf == HomeConfidence::SwitchLive || s.rotaryDisabled;
                if (wifiClient) { if (home) b = 2; else { if ((now / 500) & 1) b = 2; else r = 2; } }   // v3: home+wifi solid blue, away+wifi blue/red
                else if (home) g = 2; else r = 2;
                if (randomMode && ((now / 1000) & 1)) { b = 2; g = home ? 1 : 0; }
                break;
            }
        }
        rgbLedWrite(board::kStatusLed, r, g, b);
    }
  private:
    uint32_t last_ = 0;
};
}
#endif
