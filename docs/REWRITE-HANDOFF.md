# Uppity Spinner — handoff for a ground-up rewrite

**Written 2026-09-06, against firmware v3.6.0.** This document exists so a fresh session can
start a clean-sheet rewrite without re-deriving what took an evening of hardware debugging
to learn. Read it end to end before proposing an architecture.

Maintainer: **Todd Word** (GitHub `highfalutintodd`, Discord "Highfalutintodd").

---

## 1. What this is

ESP32 firmware for the **Uppity Spinner** periscope lifter PCB in an R2-D2 astromech dome.
The periscope is a mast that rises out of the dome and a head that rotates on top of it.

This repo (`highfalutintodd/R2UppitySpinner_ALT`) is a **community fork** of
[`reeltwo/R2UppitySpinnerV3`](https://github.com/reeltwo/R2UppitySpinnerV3) — a drop-in
replacement that preserves every original Marcduino serial command while adding a rebuilt
web UI, safer rotary homing, pulsed soft-stops, aggressiveness levels, and a serial ESTOP.

**It is not a GitHub fork** (standalone repo, no parent), so there is no "Sync fork" button.

Other people run this firmware. It is not a personal project. Design decisions must not
assume Todd's specific hardware — see §4.

---

## 2. Hardware

- **MCU:** ESP32-WROOM-32. Dual core: WiFi/web on core 0 (`eventLoopTask`), motor control in
  `loop()` on core 1. **Both cores can drive the motors today.** That is a defect, not a
  design — see §8.
- **Motor drivers:** two TB9051FTG (one lifter, one rotary).
- **Optional:** PCF8574 I2C GPIO expander. On the SD-card board variant the limit switches
  and motor-enable pins live on the expander; on the standard variant they are direct ESP32
  pins and the expander carries trigger inputs instead. `USE_SDCARD` selects this and is
  **commented out** by default — the standard variant is what ships.
- Pin assignments live in `pin-map.h`.

### 2.1 Two motors, two very different situations

This distinction caused hours of wrong reasoning. Do not repeat it.

| | Lifter | Rotary |
|---|---|---|
| Motor profile | **Yes** — `LifterMotorProfile` / `#PMOTOR` | **None whatsoever** |
| Options | Pololu 4757 (6.3:1), Pololu 4751 (19:1), IA-Parts | Builder's choice; #4847 (12V) or #4807 (6V) recommended |
| Tuning available | throttle floors, travel distance, stall detection, calibration sweep, breakaway, soft top-of-travel approach, pulse timings | `fRotaryMinPower` (default 40%), `fRotaryMinHeight` |
| Known spread | three profiles | **~250 to 1191 encoder ticks per revolution across builds — nearly 5×** |

**The boot banner names the LIFTER motor.** `Motor profile: Pololu 4751 (19:1, high-torque)`
tells you nothing about the rotary. Every field in `LifterMotorProfile` is a lifter concern,
including `fPulseOnMs`/`fPulseOffMs`, which exist for lead-screw soft-stops.

The rotary must be treated as **unknown hardware and measured at runtime**. `#PROTARYTEST`
exists for exactly this.

### 2.2 The rotary home switch is a spherical cam

Physically: a microswitch with its **lever arm removed**, mounted in a printed housing with a
**ball bearing** resting on the bare plunger. The rotating part of the periscope carries a
**pointed set screw** that rides onto the ball and presses it down.

Consequences, all measured:

- Plunger depression peaks when the screw tip is over the ball's apex and **tapers off either
  side**. Near the zone edges the plunger sits right at its actuation threshold.
- So the contact **chatters**: ~7 make/break events per pass, 22–30 raw closures per 4 passes.
  **This is geometry, not a worn or dirty switch.** Do not advise cleaning it on this evidence.
- The zone is ~14–17 ticks (~4–5°) **while moving**, but only **2–7 ticks conducting at rest** —
  vibration chatters the marginal edges closed only while the head turns.
- A single motor pulse burst moves the head **5–20 ticks**. The minimum controllable movement
  is therefore **larger than the conducting-at-rest zone**. Precise parking on this switch is
  not achievable, and any design that requires it will fail.
- First contact happens on the **opposite edge depending on approach direction**, so a naive
  stop-on-first-contact home shifts by the zone width between a CW and CCW approach.

### 2.3 Other hardware facts

- **The lifter encoder reads 0 at boot regardless of physical position.** If the mast is up
  when power is applied, the firmware thinks it is down. Infer boot position from the limit
  switches instead.
- **EMI phantom encoder ticks are real.** Pulsed motor drive next to the encoder wiring in a
  metal dome injects counts. The rotary encoder ISR counts **one edge only** (channel A rising,
  sampling B), which is the most noise-sensitive arrangement. Observed `encTicks` of -1, -4, 13
  immediately after a reset that should have left exactly 0.
- **Todd cannot turn the rotary head by hand** — the gearing prevents it without damage. The
  only manual option is laboriously spinning the encoder wheel. Any test plan that says "rotate
  the head by hand" is unusable.

---

## 3. Current firmware layout

Single large sketch, `R2UppitySpinnerV3.ino` (~7k lines), plus:

- `WebPages.h` (~2.7k lines) + `web-images.h` — web UI and JSON API
- `Screens.h`, `menus/` — optional display/menu support
- `pin-map.h` — pin assignments

### 3.1 Serial command pipeline

Marcduino grammar: `:P*` for actions, `#P*` for configuration.

- RX callbacks `onUsbSerialReceive` / `onCmdSerialReceive` run an **ESTOP FSM
  (`advanceEstopFsm`) ahead of the buffer**, then push bytes into software FIFOs
  (`sUsbFifo` / `sCmdFifo`). This is why a mid-chain `:PX` is caught even when the command
  buffer is busy — a property upstream lacks. **Keep this idea.**
- `loop()` drains the FIFOs; `runSerialCommand()` dispatches.
- Chained `:P*` commands: the programmatic chain path uses a separate `sCmdBuffer`; the
  serial-triggered path dispatches from `sBuffer`. `sProcessing` is true for the whole life of
  a chain including `:PW` waits.

Key safety commands: `:PX` (serial ESTOP), `:PH` (raise → home rotary → lower),
`:PM[G/M/A]` (random mode + aggressiveness), `#PROTARYTEST` (rotary characterisation).
Full reference is in `README.md`.

### 3.2 The safety invariant that drives everything

**The periscope will not descend below `fRotaryMinHeight` unless the rotary is home.** The
head would otherwise hit the dome. This single clamp is why a homing failure presents as
"the periscope won't retract" and parks at 66%.

Any rewrite must preserve the *intent*. How it is evaluated is discussed in §5.

---

## 4. The September 2026 rotary saga — what was actually proven

Symptom: at an event the periscope raised, would only rotate, and ignored commands.
Afterwards it would not retract, parking at 66%.

This took many rounds and several **wrong** diagnoses. The wrong turns are recorded because
they are the most useful part.

### 4.1 What was proven true

1. **The home switch works.** It fires once per revolution, every revolution, at a consistent
   position. Proven by slow-jogging the head while polling `/api/status` for `rotHome`+`rotDeg`
   and cross-checking against `loop()`'s own detections: 3 passes, 3 detections, both directions.
2. **The encoder works.** Measures 1184–1186 ticks/rev against a stored 1191, spread of 1–4
   ticks across runs. Repeatable `:PA0`/`:PA180` cycles return to the same physical spots.
3. **The pulsed creep did not move the head.** `rotateUntilHome` hardcoded 3ms on / 1ms off
   regardless of drivetrain, producing **1–2 degrees per second** — measured 2° of travel across
   three 10-second attempts. A 10-second timeout therefore searched ~10° of arc, so a switch
   anywhere else on the circle was never reached.
4. **The "precision re-approach" third pass destroyed good homes.** It backed off a home it had
   already found (`FOUND HOME`, `home=1`) and could not creep back, then fell through to the
   encoder-only fallback. Caught happening live in the logs.
5. **Contact chatter broke every post-creep check.** Single instantaneous reads said "not home"
   while the head sat squarely on the trigger. Signature: `pass2 done: deg=0 home=0`.
6. **Resting conduction cannot be relied on at all** (§2.2).

### 4.2 Wrong diagnoses, and why

- **"The encoder is drifting."** Wrong. Todd's real-world observation (same commanded angle,
  different physical position) was correct, but the cause was the *homing* corrupting the
  reference, not the encoder losing counts.
- **"The encoder is unreliable — 295% spread."** This was a **bug in the self-test**, which
  counted contact chatter as separate revolutions, producing ticks-per-rev readings of
  `{0, 0, 1186}` and a nonsense mean of 395. It then divided the contact arc by 395 instead of
  1191 and mis-scaled its own verdict. Fixed with a trailing-edge debounce.
- **"The switch is worn or dirty; clean or replace it."** Wrong. The chatter is the spherical-cam
  geometry (§2.2). Only corrected once Todd supplied a photo of the mechanism.
- **"The 19:1 gearbox is why the creep is slow."** Wrong — the 19:1 is a **lifter** option (§2.1).
  The rotary's gearing is unknown to the firmware.
- **Centring the head in the contact zone (`centerOnHome`) — failed 100% of homes.** It walked
  out to both edges and returned to the midpoint. Because the minimum movement is larger than the
  target zone, walking off was a one-way trip. Same failure class as the third pass it replaced:
  *it left a good home to look for a better one*.

**The recurring lesson: never give up a home you already have in order to improve it.**

### 4.3 What finally worked (v3.6.0)

Stop requiring parking precision the drivetrain cannot deliver:

- **Switch = edge detector while moving.** `rotateUntilHome()` returns whether contact was seen
  during the sweep. Callers succeed on that event, zero the encoder, and latch `sRotaryHomed`.
- **Encoder = position between references.** `rotaryAtHome()` answers "are we home?" from the
  encoder angle within `ROTARY_HOME_TOLERANCE_DEG` (10°) once homing has succeeded this session;
  it demands a live switch reading otherwise, so an un-homed droid cannot talk itself into lowering.
- **Creep is closed-loop**: targets a fixed angular step per pulse derived from measured
  ticks-per-revolution, adapting pulse width to achieve it. Self-tunes to any drivetrain.
- **Search limit is distance-based** (~1.25 revolutions), not wall-clock.
- `settleOntoHome()` remains as best-effort polish only; it cannot decide success.

**Result: 21 consecutive homing attempts, 21 successes**, from every kind of starting angle,
including a sustained auto-mode soak. Every retract reached the bottom limit.

---

## 5. Requirements for the rewrite

### 5.1 Homing and rotary

- Treat the home switch as a **reference event**, never a resting state. Detect it during motion.
- Make centre-finding-by-stepping a non-goal. The drivetrain cannot position that finely.
- **Never abandon a confirmed home to refine it.** Two separate implementations died on this.
- Home search must sweep by **distance** (a full revolution), not by a timeout.
- Debounce every decision-point read of the switch. Keep raw single-sample reads for tight
  polling loops and logging only.
- Characterise the rotary at runtime — ticks/rev, contact arc, chatter rate. Keep something like
  `#PROTARYTEST` as a **first-class diagnostic**, not an afterthought. It answered in one minute
  what otherwise took an evening.
- Validate calibration by **repeatability** (two consecutive readings within ~5%), not by
  magnitude floors. The old hard-coded 1000-tick floor locked out an entire class of builds.
- Give the rotary a real profile/characterisation model instead of leaving it unmodelled.
- A failed home must **not** silently re-zero the encoder and report success.

### 5.2 Motion safety

- One authority for motor cutoffs. Today core 0 (web) and core 1 (`loop()`) both drive the
  rotary, and `rotaryMotorUpdate()` can override the rescue override.
- Infer boot position from limit switches; never assume the encoder starts at zero.
- Keep ESTOP checks inside movement loops, and keep the RX-callback ESTOP FSM that catches
  `:PX` ahead of the command buffer.
- Preserve the descend-only-when-homed invariant. Be explicit about how "homed" is evaluated —
  v3.6.0 deliberately trades a live switch reading for a latched home plus an encoder angle, and
  that trade should be a conscious design decision, not inherited by accident.
- Pulsed drive is **required** for lead-screw soft-stop deceleration and **harmful** for slow
  creep. These are different problems; do not use one mechanism for both.

### 5.3 Serial

- Separate receive buffer **and** dispatcher state per source. v3.6.0 gave the command serial its
  own line buffer, but the handoff still shares `sBuffer`/`sPos` with the console, so a
  command-serial line completing while the console is mid-line truncates the console line.
- Decide acceptance **per line**: a line belongs to us only if it opens with `:` or `#`.
  Real droids share this bus with chatty peers — SABE and Roam-A-Dome both emit
  `&<NAME>,HB,...` heartbeats several times a second, and the old filter let a heartbeat's
  trailing CR execute a truncated command.

### 5.4 Diagnostics and logging

- **Logging must not alias.** The old `HOME` print had a 2-second throttle *and* was sampled once
  per `loop()`; the log could not distinguish "fires every pass" from "rarely fires", which sent
  the whole investigation down the wrong path for several rounds.
- Log messages must not overstate. `HOME (encoder only ...)` read like success when homing had
  failed.
- Fail fast and loudly. The old design could spend a minute spinning unresponsively on a bad
  switch (10s × 3 per attempt, twice per `:PH`).

---

## 6. Known bugs and backlog

### 6.1 `:PD` relative rotation — never fixed, was earmarked for "v3.5.4"

*(Todd asked for this note specifically: this is what that backlog item was about.)*

Two defects in the Marcduino `:PD<degrees>` relative-rotation command:

1. **Multi-revolution rotation is impossible.** `rotaryMotorRelativePosition()` is implemented
   as `rotaryMotorAbsolutePosition(current + relative)`, and that function calls `normalize()`
   on the target, wrapping it into [0,360). So `:PD720` from 0° becomes target 0° — an instant
   no-op. `:PD360` likewise. Anything ≥180° takes the short way round, so **~180° is the maximum
   physical rotation a single `:PD` can produce.** User-visible symptom: a chain like
   `:PP100,40:PL7:PD720,60:PH40` lifts, flashes, and immediately descends. Workaround is chaining
   multiple `:PD180` calls.
2. **The comma-speed argument is silently ignored.** The dispatcher passes only `degrees`;
   the parsed speed/maxspeed are dropped. `:PA` handles them correctly.

Why it was never done: `rotaryMotorAbsolutePosition` is shared with `rotateHome()` and `:PM`
random mode, both of which *need* the `normalize()` and shortest-path behaviour. The safe
approach is to **bifurcate** — give relative multi-revolution motion its own path computing
target encoder ticks directly (`current + relative * ticksPerRev / 360`, no normalize, no
shortest-path) with its own stall detection — and leave the absolute path untouched. In a
rewrite this is a clean design decision rather than a risky patch.

### 6.2 Rotary absolute-angle drift — cause now measured

Stored `circleEnc` is **1191**; the head actually turns **1184–1186 ticks/rev** (two
`#PROTARYTEST` runs, spreads of 1 and 4 ticks). A systematic **0.5% overestimate**, which is
roughly **18° of drift per 10 continuous revolutions** — the right order of magnitude for the
reported symptom. The stored value came from a single safety-maneuver measurement; the
self-test's averaged, debounced figure is better. A rewrite should let a confident
characterisation run update the stored calibration.

### 6.3 Other open items

- **Lifter encoder boots at 0** even with the mast physically up (§2.3).
- **`AUTO: big lift` overshoots**: `SEEK DOWN 1350->148 (11%)` ends at the bottom limit —
  at speed 76 the ramp zone cannot decelerate in time. Cosmetic, auto mode only.
- **Rescue page safety override** was dead in v3.5.3 (`sRescueOverrideExpiry` was never set from
  the page); the API path works (`/api/action?do=override_on`).
- **Builder-requested dark mode** for the web UI — must be an opt-in toggle, default light.
- **Seek-mode selector** (Gentle/Normal/Aggressive) was planned.

---

## 7. Build, flash, test

- **`arduino-cli` is installed** (1.5.1 via Homebrew) with the `esp32:esp32` core and all three
  libraries (`reeltwo/Reeltwo`, `adafruit/Adafruit_NeoPixel`, `reeltwo/PCF8574`):

      arduino-cli compile --fqbn esp32:esp32:esp32 .

  Pass `--build-path` under a scratch directory and reuse it for fast incremental builds. To
  measure a change's size impact, compile the pre-change file the same way and diff the
  "Sketch uses N bytes" line.
- **Flash headroom is tight: ~94% of program storage** (~1.24 MB of 1.31 MB). Check the size
  line on anything that adds code. This is a real constraint on the rewrite's ambitions.
- PlatformIO (`platformio.ini`, env `uppity-spinner`) and a reeltwo Arduino.mk Makefile also exist.
- **Claude cannot flash or test on hardware.** Todd does that. Write test plans as "compiles
  clean; behaviour verified on the owner's hardware", never "CI-verified".
- The repo lives under an **iCloud path containing spaces** — always quote paths.
  `build/` and `.claude/` are gitignored.

### 7.1 Useful diagnostics that already exist

- `#PROTARYTEST[1-10]` — rotary characterisation (see §5.1).
- `/api/status` — live JSON including `rotHome`, `rotDeg`, `rotPos`, limits, faults, `lastCmd`.
  Polling it in a shell loop is a far better instrument than the serial log.
- `/api/rotary?s=<-1..1>` — jogs the rotary and **bypasses the 40% minimum power floor**
  (floors at 0.10), so it can creep far slower than any serial command allows. Gives 10 seconds
  of motion per call. This is how the switch was proven good.
- `/api/action?do=override_on` then `/api/lift?t=-0.4` — lowers the mast bypassing the
  descend-only-when-homed clamp. **Only with the head visually aligned to the dome opening.**
- `/api/action?do=clearfault` — clears a failed safety maneuver so diagnostics can run.

---

## 8. Traps that cost real time

1. **Do not infer rotary behaviour from the boot banner** — it names the lifter motor (§2.1).
2. **Do not trust the firmware's own logs as instruments** without checking their sampling and
   throttling. Two independent aliasing effects made "rarely triggers" indistinguishable from
   "triggers every pass".
3. **Do not conclude "hardware is fine" from testing one component.** The switch was proven good
   and that was over-generalised to the whole subsystem.
4. **Do not build a homing refinement that can lose the home.** Twice now.
5. **Check *all* the sites when changing a magic number.** The 1000-tick rotary floor appeared at
   five, and changing a subset leaves a droid half-working with no error explaining why.
6. **Ask what the mechanism physically is.** The spherical-cam photo reframed the entire problem
   and invalidated a recommendation to replace a perfectly good switch.

---

## 9. Ecosystem

| Thing | Where | Notes |
|---|---|---|
| This sketch (ALT) | `highfalutintodd/R2UppitySpinner_ALT` | Standalone repo, not a GitHub fork |
| Upstream sketch | `reeltwo/R2UppitySpinnerV3` | Maintainer `thePunderWoman` |
| ReelTwo library (stock) | `reeltwo/Reeltwo` | Arduino dependency |
| ReelTwo library (local, **patched**) | `~/Documents/Arduino/libraries/Reeltwo/src/` | Patched for ESP32 core 3.x / ESP-IDF 5.5. **Do not overwrite.** |
| DroidNet command library | `travisccook/droidnet-command-library` | Shared board catalog; ALT board file contributed via PR |
| Todd's other boards | `highfalutintodd/sabe`, Roam-A-Dome | Share the Marcduino serial bus; emit heartbeats (§5.3) |

Ongoing side thread: **upstreaming ALT improvements** back to reeltwo and the DroidNet catalog.
Live status in `docs/UPSTREAMING.md`.

Upstream house style, if contributing there: keep **both** code paths behind
`#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)` / `ESP_ARDUINO_VERSION` guards and never drop
the legacy branch. One file per PR, independent branches off `master`, honest test plans.

---

## 10. Working with Todd

- Answer neutrally. No condescending closers, no "did you not realize" framings.
- **Public actions need explicit per-action approval** — PRs, pushes, Discord/GitHub posts.
  One approval does not carry to the next action.
- He is a capable builder debugging real hardware. His physical observations are data and have
  twice been right when the log analysis was wrong. Take them seriously.
- Keep the README's plain, builder-facing voice: explain the *why* (hardware reality), not just
  the *what*.
- Versioning: `FIRMWARE_VERSION` semver in the `.ino`; bump with the change and note it in the
  README changelog. Tags follow `vX.Y.Z — one-line summary`.
- Commits end with the Co-Authored-By trailer for Claude.
