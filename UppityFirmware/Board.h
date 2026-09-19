#pragma once
// Uppity Spinner PCB pin map — standard (non-SD) board variant.
// Every physical connection in one place. Numbers 200–207 are pins on the PCF8574
// I2C expander (bit = number - 200), never real ESP32 pins.
//
// The SD-card board variant (limit switches and motor enable on the expander) is not
// supported by v4; see docs/REWRITE-PLAN.md §1.1.
namespace board {

// ---- Lifter (raises and lowers the mast) ----
constexpr int kLifterEncA   = 34;   // encoder channel A (input-only pin)
constexpr int kLifterEncB   = 35;   // encoder channel B (input-only pin)
constexpr int kLifterPwm1   = 32;   // TB9051FTG PWM1
constexpr int kLifterPwm2   = 33;   // TB9051FTG PWM2
constexpr int kLifterDiag   = 36;   // TB9051FTG DIAG, LOW = fault (input-only, no internal pull-up)
constexpr int kLifterTop    = 18;   // top limit switch
constexpr int kLifterBottom = 19;   // bottom limit switch

// ---- Rotary (spins the head) ----
constexpr int kRotaryEncA   = 27;
constexpr int kRotaryEncB   = 13;
constexpr int kRotaryPwm1   = 25;
constexpr int kRotaryPwm2   = 26;
constexpr int kRotaryDiag   = 39;   // LOW = fault (input-only, no internal pull-up)
constexpr int kRotaryHome   = 23;   // home switch (spherical cam on a lever-less microswitch)

// ---- General ----
constexpr int kStatusLed    = 5;    // single NeoPixel
constexpr int kCmdRx        = 16;   // command UART (Marcduino bus) RX
constexpr int kCmdTx        = 0;    // command UART TX — GPIO0 is a boot strap; only driven if wired
constexpr int kI2cSda       = 21;
constexpr int kI2cScl       = 22;
constexpr int kExpanderInt  = 17;   // PCF8574 INT, active low
constexpr uint8_t kExpanderAddr = 0x20;

// ---- PCF8574 expander bits ----
constexpr uint8_t kExpInputA    = 0; // external trigger input A
constexpr uint8_t kExpLightA    = 1; // light kit mode bit 2 (MSB), active low
constexpr uint8_t kExpLightB    = 2; // light kit mode bit 1
constexpr uint8_t kExpLightC    = 3; // light kit mode bit 0 (LSB)
constexpr uint8_t kExpMotorEn   = 4; // motor enable, HIGH = drivers on
constexpr uint8_t kExpMotorEnB  = 5; // motor enable inverted, LOW = drivers on
constexpr uint8_t kExpInputB    = 6; // external trigger input B
constexpr uint8_t kExpInputC    = 7; // external trigger input C

// ---- Electrical choices (see docs/TB9051FTG-NOTES.md) ----
// The TB9051FTG accepts PWM up to 20 kHz but needs >= 10 us high AND low per cycle, with up to
// 13 us of input delay. At 10 kHz the 12-88 % band honours that; at 20 kHz only 20-80 % would.
constexpr uint32_t kPwmHz        = 10000;
constexpr uint8_t  kPwmBits      = 10;    // 0..1023
constexpr uint32_t kPwmMinHighUs = 12;    // shortest pulse we will ask the driver to resolve
constexpr uint32_t kPwmPeriodUs  = 1000000UL / kPwmHz;
constexpr int      kPwmMinDutyPermille = static_cast<int>(kPwmMinHighUs * 1000 / kPwmPeriodUs);        // 120
constexpr int      kPwmMaxDutyPermille = 1000 - kPwmMinDutyPermille;                                   // 880

} // namespace board
