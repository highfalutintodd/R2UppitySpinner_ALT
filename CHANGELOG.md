# Changelog

## 4.0.0-beta.1 — 2026-10-24 (pre-release for testers)

A clean-sheet rewrite, in `UppityFirmware/`. Every v3 `:P…` and `#P…` command keeps its syntax.
Nothing is copied from the v3 code or the ReelTwo library.

**New**
- One motion controller: every other part of the firmware asks, only it drives, anything can stop.
  Every motion is a step-by-step state machine, so the page, the serial ports and E-STOP stay live
  during calibration and homing.
- One set of safety rules: the head never descends unless it is home, never turns below the
  measured safe-spin height, and an unknown mast height allows only upward moves.
- E-STOP latches until released (page Reset, `#PRESUME`, mesh `&ESTOP,RESET`); while latched it
  brakes until both motors are still, then turns the drivers off.
- Commissioning: a seven-step checklist that measures the lifter, the rotary and the safe-spin
  height before the motors are unlocked; resumable; one expert bypass.
- Homing treats the home switch as a click while the head sweeps past it, and learns the rotary's
  real ticks per turn.
- Seeks brake by the speed they actually reached, from the board's own calibration: short moves
  move, and moves land within a few ticks.
- A new web interface (Operate, Setup, Diagnostics, Rescue) in the Sabé look, with firmware
  upload, a rescue override, hold-to-jog and measured nudges. E-STOP from the page lands
  mid-move.
- WiFi: the board's own network always up, an optional home network, `http://uppity.local`.
  The network name comes from the board's factory address (`R2Uppity-XXXX`), and the page and
  the boot log warn while the published default password is in use.
- Optional WCB mesh membership (off by default): `&PERI` heartbeat, position, state and fault
  messages; moves, E-STOP and status questions by board number; settings and the rescue override
  are refused from the mesh.
- v3's saved settings are copied once on the first boot after an upgrade that keeps them.
- `tools/release.sh` builds the install files: a full image, an update for the page, and the four
  files for a v3 upgrade.

**Changed from v3**
- `:PH` stops raised and says why when the home switch is not found, instead of looping.
- No safety maneuver on the first command after power-up.
- A turned head sent down stops at the safe-spin height; a second descent waits for a home.
- The first install from v3 is a USB flash (the flash is split differently, with room to grow).

**Removed:** the SMQ Droid Remote and OLED menus, RC/PPM input, the SD-card board variant,
Marcduino over WiFi (TCP 2000), PlatformIO and the reeltwo Makefile, the patched ReelTwo library.

**Tested:** on one droid (19:1 inverted lifter, rotary about 1,185 ticks per turn, Sabé, three
WCBs on 7.0.1) through the bench procedures in `docs/BENCH-PHASE1.md` … `BENCH-PHASE5.md` and
`BENCH-BETA.md`, the last including the command serial port driven by a WCB, the same command over
the serial bus and the mesh (runs once), E-STOPs over the mesh from a WCB and from Sabé, and a
fresh install from the full image followed by commissioning from scratch; 235 host tests on a
simulated periscope. **Not yet run on hardware:** the 6.3:1 and IA-Parts lifters, other builders'
rotaries, and the trigger inputs.

**Known gaps:** no sequence editor on the page (stored sequences work over serial); v3's stored
sequences are not copied.

## v3 (the firmware in the repository root)

### v3.6.0 — Rotary homing rebuilt around what the hardware can actually do

Homing had become unreliable to the point where the periscope would not retract: a failed
home blocks descent below the safe rotary height, so the mast parked at 66% and stayed
there. Diagnosing it turned up several separate faults.

**The home switch is a spherical cam.** A pointed set screw on the rotating head presses a
ball bearing into a microswitch whose lever has been removed. Plunger depression peaks at
the ball's apex and tapers off either side, so near the edges of the contact zone the switch
sits right at its actuation threshold and chatters — around seven make/break events per
pass. That is geometry, not a worn switch. It also means the switch only holds closed within
a couple of encoder ticks of centre, which is **finer than the drivetrain's smallest
movement** (a single pulse burst shifts the head 5–20 ticks).

- **The switch is now an edge detector, not a resting state.** Homing succeeds when the
  switch fires *while sweeping past it*; the encoder is zeroed at that moment and a
  "homed" latch is set. "Are we at home?" is then answered from the encoder angle, which
  measures 1184–1186 ticks/rev against a stored 1191 — inside 2%. Previously the code
  demanded a live switch reading after stopping, which the mechanism cannot reliably give,
  so good homes were thrown away. An un-homed droid still requires a real switch reading
  before it will lower.
- **The creep is closed-loop on the encoder.** It used to pulse 3ms on / 1ms off, which
  produced 1–2 degrees per second on the test droid — the 10-second timeout expired after
  about 10 degrees of arc and the switch was never reached. It now targets a fixed *angular*
  step per pulse, derived from your own measured ticks-per-revolution, and adapts the pulse
  width until it achieves it. **The rotary has no motor profile** (the `#PMOTOR` profiles are
  lifter-only) and measured rotary revolutions across builds run from ~250 to 1191 ticks, so
  nothing here is tuned to one droid.
- **Home search gives up by distance, not by clock** — about 1.25 revolutions, so every
  droid does the same amount of searching.
- **Removed the "precision re-approach" third pass.** It backed off a home it had already
  found and could not creep back, converting successes into failures.
- **Pass 1 brakes instead of coasting**, so the head cannot drift off a switch it just
  reached.
- **Removed a hard-coded 1000-tick floor** on the rotary revolution count, present at five
  sites. Builders whose rotary measures fewer than 1000 ticks/rev — one community build
  measures ~250 — had the safety maneuver reject the reading as bad, retry, and abort. The
  floor is now 100 ticks in one named constant.
- **New `#PROTARYTEST[1-10]`.** Spins the head slowly and reports ticks per revolution and
  its repeatability, the home switch's contact arc, raw contact-closure count, and whether
  the creep step fits inside the contact window. **Run this first** if homing misbehaves.
- **Command serial no longer trips over other boards' chatter.** Peers on the Marcduino bus
  (SABE, Roam-A-Dome and similar) emit `&<NAME>,HB,...` heartbeats several times a second.
  The old filter only ignored foreign bytes while the shared buffer was empty, so a
  heartbeat arriving mid-command was appended to it — and its trailing carriage return then
  executed whatever that produced. Foreign lines are now discarded whole, and the command
  serial assembles into its own buffer so it can never mix with the console.
- **Honest logs.** A failed home used to print `HOME (encoder only ...)`, which read like
  success. Removed a leftover `ch: X [nn]` debug print that echoed every command byte.

Verified on hardware: 21 consecutive homing attempts from a wide spread of starting angles,
all successful, every retract reaching the bottom limit, including a sustained auto-mode
soak at Medium and Aggressive.

### v3.5.3 — Chained `:P*` commands actually run
- **Chain dispatcher fix.** Chained Marcduino lifter commands past the first one were silently no-op'ing — `:PL7:PP100` set the lights but never raised the periscope, `:PP100:PL5` raised the periscope but never changed the lights, and so on for every `:PA`, `:PD`, `:PH`, `:PW` chained after a first command. Root cause: the chained-command path stripped only the leading `:` before dispatching, but the dispatcher's switch consumes the next character as the command letter — so the `P` in `:PP100` got eaten as the verb, and the parser then tried (and failed) to read `P100` as a position number. The fix mirrors the firstCommand path: skip `:P`, parse the optional ID, then dispatch. The bug had been present since the original upstream code; this fork's earlier "chain refactor" only touched the safety-maneuver pre-gate and didn't fix this part. Behavior change is strictly additive — chains that worked still work; chains that previously vanished now execute.
- **README chain-rules examples** updated to match the fixed behavior. The old `:PM:PL5` ✗ example was reasoning from the broken dispatch path; replaced with examples that distinguish blocking motion (`:PP`/`:PA`/`:PD`/`:PH` — chained commands wait) from non-blocking move-mode start (`:PM` — chained commands fire immediately after).
- **Fork attribution in the `.ino` header.** Added a `Forked by highfalutintodd` line beneath Mimir Reynisson's authorship line so builders flashing this fork don't think they're running upstream firmware. Mimir's authorship and the LGPL block are unchanged.

### v3.5.2 — Expert mode for the 19:1 lifter
- **Optional 100% throttle on the 19:1 motor.** The Pololu 4751 19:1 profile ships with `fMaxCommandedThrottle = 0.75` and a calibration sweep cap of 85%, both there to keep mid-bring-up frames from cracking under full power. A new opt-in **Expert Mode** toggle lifts both ceilings to 1.0 / 100% — but only when the active motor is the 19:1. For every other motor profile, the toggle is a no-op (and the Parameters card hides itself).
- **Three control surfaces:** Parameters page checkbox (with a frame-damage `confirm()` modal on enable + a re-calibration banner that links to `/calibrate`), `#PEXPERT0` / `#PEXPERT1` Marcduino command (with the same warning text on serial), and `expert` field in the `/api/params` JSON. State persists to flash.
- **Wizard creep deliberately untouched.** `wizardCreepToLimit` still uses the profile-safe 0.75 ceiling so the breakaway-friction-validation step the wizard relies on doesn't get unstable at 100%. Only the user-commanded motion path, calibration sweep, and aggressive-random upper bound respect Expert Mode.
- **Graceful re-calibration.** Enabling Expert Mode without re-running calibration is safe — seeks above 85% just fall back to the highest calibrated slot (from the v3.5.0 Speed=100 fallback work). The web UI surfaces a yellow re-cal banner so it's not a silent step.

### v3.5.1 — Serial ESTOP (`:PX`) + chain & web fixes
- **Real mid-motion ESTOP from serial.** New `:PX` command aborts any in-flight seek, rotation, or random action and stops both motors. Detection runs in the UART RX callback (separate task from the motor-control loop), so it fires the abort flag the moment the bytes arrive — even while a `seekToPosition` is blocking. Works on USB Serial and on the Marcduino UART2 input.
- Dispatcher-path `:PX` (e.g. inside a stored sequence) logs `ESTOP` and ends move mode for clean state.
- Refactored serial reading: bytes now flow UART → onReceive callback → software FIFO → `loop()` dispatcher. The callback scans for `:PX<EOL>` with a 4-state machine and sets the existing `sWebAbort` flag the same way the web E-STOP does.
- **Chain dispatcher: dropped the safety-maneuver pre-gate.** Previously, every chained-after-the-first command was gated on `sSafetyManeuver==true`, which meant chains like `:PL7:PP100:...` aborted on a fresh boot because `:PL` (lights) doesn't trigger the maneuver and the chained `:PP100` was rejected before it could. Motion handlers already call `ensureSafetyManeuver()` themselves, so the pre-gate was redundant — and it was incorrectly blocking non-motion chained commands like `:PL` and `:W`.
- **Web E-STOP now actually stops calibration.** The calibration sweep ran nested loops; the inner break consumed `sWebAbort`, but the outer speed-sweep `for (...; sCalibrating && ...)` kept right on going. `serialAbort()` now also clears `sCalibrating` so all nested loops bail in the same pass.
- **Desktop nav rendered server-side.** The sidebar nav was being built by inline JS only when `window.innerWidth>=1024`. When that script didn't fire (some browser/timing setups), desktop users got an empty sidebar and the bottom tab bar was hidden by CSS — so no visible nav at all. The sidebar HTML is now emitted by the firmware directly; CSS alone toggles between bottom tab bar (phone/tablet) and sidebar (desktop). No JS, no race.

### v3.5.0 — Aggressiveness levels + Parameters page rebuild
- **Selectable random aggressiveness.** The auto-move mode now has Gentle / Medium / Aggressive profiles that scale lift speed bands, rotary speed bands, action mix, and inter-action delay. Gentle skips the two highest-energy switch cases entirely. Rotary safety is preserved at every level — every descent path still homes the rotary first regardless of aggressiveness.
- **Per-invocation overrides.** New `:PMG` / `:PMM` / `:PMA` Marcduino commands launch a one-shot run at the specified level without touching the global default. New Periscope-tab buttons (Gentle / Medium / Aggressive / Stop) call a `/api/movemode?level=g|m|a` endpoint.
- **Global default in Preferences.** The Parameters page has a new Aggressiveness dropdown that sets what bare `:PM` and the "random (default)" web button use. Settable from serial via `#PAGGRESSION<n>` (0/1/2). Persisted to flash.
- **Parameters page rebuilt.** Restyled to match the Calibrate / Periscope card layout — motor thresholds, safe-spin height (with "use current" capture), drift correction, rotary disable, aggressiveness, and load-defaults buttons all in one consistent UI. Backed by a new JSON `/api/params` + `/api/params/save` pair.
- **Speed=100 no longer silently fails.** Seeks now fall back gracefully to the highest calibrated slot when the requested speed lands above the calibration sweep cap (e.g. requesting 100% on a 19:1 motor whose calibration intentionally tops out at 85%). The Periscope-tab lift speed slider also auto-caps to the active motor's `maxThrottle` so you can't slide past what the motor profile allows.
- **Lifter direction invert** exposed in the web UI for builds where the lifter wiring runs in reverse.

### v3.4.0 — Multi-motor support + first-run wizard
- **19:1 lifter motor support.** Added user-selectable motor type (Pololu 4757 6.3:1 / Pololu 4751 19:1 / IA-Parts). Each motor ships with its own tuned profile: throttle ceiling, stall timeout, pulse timing, calibration sweep cap, soft-approach ramp. The 19:1 profile is locked at 0.75 throttle with a 700ms stall timeout and 15% top-of-travel ramp to prevent the frame damage Greg Hulette hit during early 19:1 bring-up.
- **First-run setup wizard** (`/wizard`). Motion is gated until the builder walks through motor select → sensor check → low-power creep test → calibration → acceptance seeks. Existing calibrated installs auto-promote on boot so firmware upgrades don't force a re-calibration.
- **`#PMOTOR<n>` Marcduino command** to query or switch motor type. `#PCONFIG` now reports active motor profile and wizard state.
- Stall timeouts in calibration and seek-to-bottom paths now honor the active motor profile instead of hard-coded 2000ms.

### v3.3.5 — Code-review fixes
- Fixed duplicate `rotateHome()` call causing the rotary home-spam bug.
- `volatile`-qualified `sRotaryCircleEncoderCount` to protect the cross-core read.
- Widened stall-detect tick-rate math to int64 to prevent integer overflow on long-duration seeks.
- ESTOP checks added inside rotary creep loops so `/api/estop` halts active homing sequences.
- Snapshot `sRescueOverrideExpiry` before comparing to avoid torn cross-core reads.

### v3.3.4 — Initial fork
Baseline fork of reeltwo/R2UppitySpinnerV3. The v3 feature set is described in the README at tag `v3.6.0`.
