# R2 Uppity Spinner ALT (v3.3.4)

A fork of [reeltwo/R2UppitySpinnerV3](https://github.com/reeltwo/R2UppitySpinnerV3) — the ESP32-based periscope lifter and rotary controller for R2-D2 astromech droids.

This fork focuses on **robustness for real-world droid builds**: tighter lead-screw mechanisms, high-friction belt drives, metal dome EMI, and field recovery. All original Marcduino serial commands and web interfaces are fully preserved — this is a drop-in replacement for the stock firmware.

[![Uppity Spinner](https://i.vimeocdn.com/video/1153816619-6fef8819cf32b562e0519537a46baed562bb51651010442a9ccdd9909c40952e-d_640x360)](https://vimeo.com/558277516)

## What's Different from Stock

### Bug Fixes

**ESTOP actually stops everything now.** The stock web ESTOP (`/api/estop`) sets an abort flag, but rotary homing functions didn't check it — the rotary would keep spinning after ESTOP until it finished its homing sequence. This fork adds abort checks to `rotateUntilHome()`, `rotateLeftHome()`, `rotateRightHome()`, and `rotaryMotorAbsolutePosition()`. Additionally, the abort flag is now properly cleared after ESTOP so the next command isn't silently consumed.

**Stall detection works on slow motors.** The stock `seekToBottom()` uses `LifterStatus` to detect stalls, which requires 20+ encoder ticks per 200ms. Greg Hulette lifters at 40% power produce only ~15 ticks/200ms, causing false stall aborts that skip encoder re-zeroing and corrupt position tracking. This fork replaces it with a 2-second position-change timeout — any movement resets the timer, so even slow motors work correctly.

**Rotary precision creep actually varies with speed.** The stock `rotateUntilHome()` formula `0.1 * abs(speed)` squashes the input range so that `speed=0.01` and `speed=0.1` produce nearly identical motor speeds. This fork uses `ROTARY_MINIMUM_POWER/100.0 + abs(speed)` so low-speed homing commands produce meaningfully different approach speeds.

### New Features

**Breakaway boost.** Lead-screw mechanisms with tight belts or 3D-printed mounts can have significant static friction. The stock pulsed drive pattern (3ms on / 1ms off) may not overcome it from a standstill. This fork adds a two-phase startup:
1. **Continuous burst** — runs the motor without pulsing until the encoder confirms real movement (default: 10 ticks or 500ms timeout).
2. **Ramp to pulsed** — gradually transitions from continuous drive to the standard pulsed pattern over a distance proportional to the seek length.

Short seeks (< 30 ticks) skip the boost entirely. The ramp scales to `min(80, seekDistance/3)` ticks, preventing momentum buildup on short moves while still providing enough kick for long ones. Breakaway is used in both UP and DOWN seek loops, with automatic re-boost if slow progress is detected mid-seek.

**Firmware versioning.** A `FIRMWARE_VERSION` define (currently `"3.3.4"`) is displayed:
- Serial boot banner: `R2UppitySpinner v3.3.4`
- ALIVE heartbeat (every 60s): `ALIVE v3.3.4`
- `#PCONFIG` output: `Firmware: 3.3.4`
- Web page titles and status bar (6th column)
- `/api/status` JSON: `"version":"3.3.4"`

**Expanded web status bar.** The status bar is now 6 columns:
| Height | Rotation | Safety | Motors | Last Command | Firmware |
|--------|----------|--------|--------|-------------|----------|

The rotary field shows degrees (0–359) or "home" when near 0°.

### Enhanced Safety Systems

**Pulsed drive for lead screws.** All vertical motion uses a 3ms ON / 1ms OFF pulse pattern instead of continuous PWM. Lead screws self-lock when power is removed, so each pulse moves ~1 encoder tick and the mechanism brakes mechanically in the off period. This prevents violent limit switch impacts that continuous drive causes when momentum builds up.

**Distance rejection.** During calibration and safety maneuver, measured lifter distance is validated against the stored calibrated value. Measurements under 50 ticks or differing more than 20% from stored are rejected — this guards against EMI-corrupted encoder readings caused by metal domes.

**Encoder drift correction.** After a seek completes, the firmware holds the target position. Every 5 seconds it checks if the encoder has drifted beyond a configurable threshold (default 5%, adjustable via `driftpct` preference, range 0–20%) and re-seeks if needed. Gives up after 3 consecutive failures to prevent infinite retry loops from persistent EMI.

## Hardware

Same hardware as stock — no changes required.

### Microcontroller

**ESP32-WROOM-32** (or compatible). Dual-core: Core 0 handles WiFi/web, Core 1 handles motor control.

### Motor Drivers

Two **TB9051FTG** drivers — one for lifter, one for rotary.

### Pin Assignments

| Function | Pin |
|----------|-----|
| Lifter Encoder A/B | 34 / 35 |
| Lifter PWM1 / PWM2 | 32 / 33 |
| Lifter DIAG | 36 |
| Lifter Top Limit | 18 |
| Lifter Bottom Limit | 19 |
| Rotary Encoder A/B | 27 / 13 |
| Rotary PWM1 / PWM2 | 25 / 26 |
| Rotary DIAG | 39 |
| Rotary Home Limit | 23 |
| Status LED (NeoPixel) | 5 |
| RC PPM Input | 14 |
| Marcduino Serial RX | 16 |
| I2C SDA / SCL | 21 / 22 |
| PCF8574 Interrupt | 17 |

When using the I2C GPIO expander (PCF8574 at address 0x20), limit switches and motor enable pins are remapped to the expander, freeing ESP32 pins 18/19/23 for SPI (SD card).

### Recommended Motors (Pololu)

| Voltage | Lifter | Rotary |
|---------|--------|--------|
| 12V | [#4841](https://www.pololu.com/product/4841) | [#4847](https://www.pololu.com/product/4847) |
| 6V | [#4801](https://www.pololu.com/product/4801) | [#4807](https://www.pololu.com/product/4807) |

6V motors give a snappy, fast periscope. 12V motors give a slower, more deliberate motion.

### Supported Lifter Mechanisms

| Parameter | Greg Hulette | IA-Parts |
|-----------|-------------|----------|
| Minimum Power | 65% | 30% |
| Seek-to-Bottom Power | 40% | 30% |
| Full Travel Distance | ~450 ticks | ~845 ticks |

Both are auto-detected during calibration.

## Libraries Required

**Important:** This fork requires a patched version of Reeltwo for ESP32 Arduino core 3.x / ESP-IDF v5.5 compatibility. All three required libraries are included in the `libraries/` folder of this repo — copy them to your Arduino `libraries/` folder (or symlink them).

- **Reeltwo** (patched) — Core droid framework (WiFi, web server, OTA, SMQ remote)
- **Adafruit NeoPixel** — RGB status LED
- **PCF8574** — I2C GPIO expander (optional)

## Getting Started

1. Wire the ESP32, motor drivers, encoders, and limit switches per the pin table above.
2. Copy the three folders from `libraries/` in this repo into your Arduino `libraries/` folder (replacing any existing versions).
3. Flash the firmware via USB or OTA.
4. Connect to the WiFi access point (default SSID: `R2Uppity`, password: `Astromech`).
5. Open `http://192.168.4.1/` in a browser.
6. Run calibration from the Setup page or send `#PSC` over serial.

Default serial baud rate is **9600**. Default I2C address is **0x20**.

## Serial Commands

All stock Marcduino commands are supported with no changes to syntax.

### Lifter Commands

Commands start with `:P` followed by a subcommand. Multiple commands can be chained with colons:

    :PP100:W2:P0    (raise, wait 2s, lower)

| Command | Description |
|---------|-------------|
| `:PP<0-100>[,speed]` | Move lifter to position % (0=down, 100=up) |
| `:PPR[,speed]` | Random position |
| `:PH[speed]` | Home (rotate to 0deg, lower to bottom) |
| `:PA<degrees>[,speed][,maxspeed]` | Rotate to absolute degrees |
| `:PAR[,speed][,maxspeed]` | Random absolute rotation |
| `:PD<degrees>[,speed][,maxspeed]` | Rotate relative degrees (+CCW, -CW) |
| `:PDR[,speed][,maxspeed]` | Random relative rotation |
| `:PR<speed>` | Continuous spin (+CCW, -CW). 0 = stop/home |
| `:PM[,liftSpd,rotSpd,minInt,maxInt]` | Random animation mode |
| `:PW[R]<seconds>` | Wait (R = randomize 1..N) |
| `:PL<0-7>` | Light kit mode |
| `:PS<0-100>` | Play stored sequence |

### Configuration Commands

| Command | Description |
|---------|-------------|
| `#PSC` | Run calibration (stores results to preferences) |
| `#PZERO` | Erase all stored sequences |
| `#PFACTORY` | Factory reset (clear all preferences and sequences) |
| `#PL` | List stored sequences |
| `#PD<n>` | Delete sequence n |
| `#PS<n>:<seq>` | Store sequence (e.g. `#PS1:H`) |
| `#PID<n>` | Set lifter ID (0-255) for multi-lifter systems |
| `#PBAUD<rate>` | Change baud rate (persistent, takes effect after reboot) |
| `#PR` | Toggle rotary enable/disable (reboots) |
| `#PNCL` / `#PNOL` | Set lifter limit switch normally closed / normally open |
| `#PNCR` / `#PNOR` | Set rotary limit switch normally closed / normally open |
| `#PCONFIG` | Display full configuration (now includes firmware version) |
| `#PSTATUS` | Show WiFi/remote status |
| `#PWIFI[0\|1]` | Enable/disable WiFi (toggle if no arg) |
| `#PWIFIRESET` | Reset WiFi to defaults and reboot |
| `#PREMOTE[0\|1]` | Enable/disable Droid Remote (reboots) |
| `#PRNAME<host>` | Set Remote hostname (reboots) |
| `#PRSECRET<secret>` | Set Remote shared secret (reboots) |
| `#PRPAIR` | Start 60s pairing window |
| `#PRUNPAIR` | Unpair all remotes (reboots) |
| `#PDEBUG[0\|1]` | Enable/disable verbose debug output |
| `#PRESTART` | Reboot |

### Stored Sequence Examples

    #PS1:H                        (home)
    #PS2:P100                     (periscope up)
    #PS3:P100:L5:R30              (search light, spin CCW)
    #PS4:P100,100:L7:M,80,80,2,4 (random fast with sparkle)
    #PS5:P100:L7:M,50,40,5,5     (random slow with sparkle)
    #PS6:A0                       (face forward)
    #PS7:P100:L5:R-30             (search light, spin CW)
    #PS8:H:P50:W2:P85,35:A90,25:W2:A270,20,100:W2:P100,100:L5:R50:W4:H  (sneaky periscope)

## Web Interface

Access the web UI at `http://192.168.4.1/` (AP mode) or `http://R2Uppity.local/` (station mode with mDNS).

All stock web pages are preserved. Key additions:
- Status bar now has 6 columns (added firmware version)
- `/api/status` JSON includes `"version"` field
- Page titles include firmware version

## Troubleshooting

### EMI from Metal Dome

Metal domes cause electromagnetic interference on encoder signals, leading to undercounted ticks, phantom negative positions, and false limit triggers. Mitigations:

- **Hardware:** Twist encoder and limit switch wires tightly. Shield or route wires away from metal surfaces.
- **Software (built-in):** Position clamping prevents negative values. Distance rejection guards against corrupted calibration. Double encoder reset with settling delay at limits. Drift correction with failure limit.

### Periscope Won't Lower

If the rotary isn't homed, the safety system blocks descent. Use the Rescue page's safety override (3-second long-press) to bypass for 60 seconds.

### Lifter Stalls or Hesitates on Startup

If your lead screw has high static friction (tight belt, 3D-printed mount), the breakaway boost should handle it automatically. Check serial output for `BREAKAWAY` log lines — you should see Phase 1 (burst) and Phase 2 (ramp) completing successfully. If it consistently times out, your minimum power setting may need to be higher (adjustable on the Parameters web page).

### Calibration Distance Seems Wrong

If the measured distance differs >20% from the previously stored value, it's automatically rejected. Run calibration with the dome removed to get a clean measurement, then the stored value will be used even under dome EMI.

## Assembling the PCB

### Part 1
[![Part1](https://img.youtube.com/vi/x4_3irdV4C8/hqdefault.jpg)](https://www.youtube.com/watch?v=x4_3irdV4C8)

### Part 2
[![Part2](https://img.youtube.com/vi/MdSRJsYx1T8/hqdefault.jpg)](https://www.youtube.com/watch?v=MdSRJsYx1T8)

## License

See [LICENSE](LICENSE) for details.
