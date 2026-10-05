# Uppity Spinner ALT v4 — clean-sheet rewrite plan

---

# Part 1 — The plain-English version

This is the same plan as Part 2, written for the owner rather than for the next coding session.
Part 2 is the technical reference; if the two ever disagree, Part 2 wins and Part 1 gets fixed.

## What we are going to do

We are going to write a brand-new firmware for the Uppity Spinner board, in the same GitHub
repo, and call it version 4. It will accept every serial command the current firmware accepts,
so nothing in your Marcduino sequences, Sabé buttons, WCB stored commands or the DroidNet
catalog has to change. Underneath, nothing is copied from Skelmir's code or from the ReelTwo
library. The patched ReelTwo library goes away entirely.

The heart of the new firmware is one "traffic controller" for the motors. Today two different
parts of the chip can both drive the motors and neither knows what the other is doing; that is
why the rescue jog gets cancelled a few milliseconds after you press it, and why the wizard's
own motion can make the web E-STOP button unreachable. In the new design exactly one part of
the program is allowed to make a motor move. Everything else (serial, web page, mesh, stored
sequences, random mode) drops requests into a queue and reads back a status snapshot. Anything
is allowed to *stop* the motors at any time; only the controller can *start* them.

That controller never waits. Every motion (a seek, a home search, a calibration) is written as
a step-by-step state machine that advances a little every millisecond, instead of a loop that
holds the whole chip hostage until it finishes. The practical effect: the web page keeps
updating during calibration, an E-STOP from anywhere lands within about a millisecond, and a
`:PX` typed mid-sequence is caught before the sequence even sees it.

Homing is rebuilt around what the September debugging proved: the home switch is a reliable
"click" while the head sweeps past it, and an unreliable resting contact. The new firmware
treats it as a click, remembers where the click happened, and uses the encoder to know where it
is from then on. It never throws away a home it already found in order to look for a better
one. It will also learn your rotary's real ticks-per-revolution and keep it, and it will tell
you loudly (not quietly re-zero) when a home search fails.

The firmware will be testable on the Mac before it ever touches the periscope. We build a
simulated periscope (a fake lifter with friction and momentum, a fake rotary with a chattering
cam switch, fake limit switches, fake noise on the encoder wires) and run the real control code
against it. Every failure we have already lived through becomes a test that has to pass before
a build is offered for flashing.

The web interface is rebuilt from scratch in the same look as Sabé, with separate pages for
the three different people who use it: the operator at an event (big buttons, presets, lights,
sequences, and nothing else), the builder on the bench (commissioning, calibration, tuning,
WiFi, firmware), and whoever is rescuing a stuck periscope (E-STOP, override, jog, unlock).
Diagnostics get their own page instead of being pinned under the operator controls. The
first-run wizard becomes a commissioning checklist that remembers where you were, cannot block
E-STOP, includes the rotary self-test as a step, and has exactly one "I know what I'm doing"
bypass. Stored sequences get a proper editor so the sequence buttons finally do something.

Finally, the periscope becomes a proper citizen of the droid's wireless network. Optionally
(off by default, so other builders are not affected), it joins the WCB mesh as its own device,
receives commands directly instead of through a WCB serial port, and sends heartbeat, position
and fault messages that Sabé and DroidNet can see, in the same `&NAME,…` style Sabé and
Roam-A-Dome ALT already use.

## What stays the same for you and for other builders

- Every `:P…` and `#P…` command, chaining with colons, the ID prefix, the light kit modes, the
  random mode with Gentle/Medium/Aggressive, the three lifter motor profiles, the expert mode
  for the 19:1, the WiFi access point name and password, the web address 192.168.4.1, web
  firmware updates, the status LED, and the three external trigger inputs.
- The board. No hardware changes. The SD-card variant is dropped; anyone on it stays on v3.

## What you will notice is different

- **E-STOP now latches.** After `:PX`, the web E-STOP, or a mesh stop, the periscope stays
  stopped until you deliberately release it (a Reset button, `#PRESUME` over serial, or the
  mesh reset Sabé can send). A queued sequence step can no longer restart motion a moment
  after you hit stop. Any controller button that sends `:PX` needs a second button for the
  reset, or Sabé sends it for you.
- **Every board goes through commissioning once after the upgrade**, and neither motor moves
  until it passes (or you take the bypass). Old calibration does not carry over, partly by
  design and partly because it turns out v3 may never have been saving it to flash at all.
- **The upgrade itself is a USB flash**, not an over-the-air update, because the flash layout
  changes to give the firmware more room. After that, OTA works again.
- `:PH` no longer loops raise-home-lower for a minute when the home switch is not found; it
  stops raised, says why, and waits for you.
- `:PD` (relative rotation) really turns the number of degrees you ask for, including full
  revolutions, and honours the speed argument.
- There is no "safety maneuver" on the first command after boot. The firmware works out where
  the lifter is from the limit switches, and if it cannot tell, it only allows upward moves
  until it can.

## How the work is staged

1. **Skeleton and simulator.** The new folder layout, build profile, pin definitions, the fake
   periscope, and the test harness. Nothing runs on the board yet.
2. **Motion core, USB console only.** The motor controller, homing, seeks, calibration,
   E-STOP, faults. First hardware tests: dome off, hands clear, one command at a time.
3. **Command layer and settings.** The command serial port, chains, stored sequences, random
   mode, light kit, trigger inputs, saved settings.
4. **WiFi and a minimal web page.** Access point, updates, the Operate page.
5. **The full web interface.** Sequences editor, Setup and commissioning, Diagnostics, Rescue.
6. **Mesh and DroidNet.** Joining the WCB network, the `&PERI` messages, the updated catalog
   entry.
7. **Release.** README in builder voice, migration notes, a merged image for fresh boards, a
   tool to carry your v3 tunables and sequences across, and a beta release.

Each stage ends with the code compiling clean, the Mac tests passing, and a written bench
procedure for you. v3.6.0 stays flashable the whole time as the fallback for events.

## Concerns and things that could go wrong

**Hardware behaviour we are assuming rather than measuring.**
- *Brake versus coast.* The old code assumes that setting both motor-driver inputs low lets
  the motor coast. The driver's datasheet may say that state is actually a brake. It changes
  nothing about how the real hardware has been behaving (we keep the same pin states), but it
  changes how the simulator should model deceleration and which "off" state the pulsing should
  use. One minute with the TB9051FTG datasheet settles it; it is the first thing to check.
- *PWM frequency.* The old firmware drives the motors with a 1 kHz PWM, which is coarse enough
  that a 3 ms pulse gets a different amount of energy depending on where in the cycle it
  starts. We move to 10 kHz (the datasheet's 10 µs minimum pulse rules out 20 kHz for the low
  and high duties we need). Motors and drivers can behave a little differently at a new
  frequency, the minimum-power numbers you know will shift, and there may be a faint whine.
  Commissioning re-measures the numbers automatically.
- *Encoder reading.* We switch from interrupt counting to the ESP32's hardware pulse counter,
  which can filter out the short electrical spikes a metal dome induces. If it misbehaves on
  the real board, the old interrupt method sits behind the same interface as a fallback. This
  needs testing with the dome on, because that is where the noise lives.

**The simulator is not the periscope.** It will catch logic mistakes (the class of bug that
caused the September failure) but not surprise physics. Bench testing stays mandatory, every
phase has a procedure, and the first motion tests happen with the dome off and the head clear.
"Passes on the Mac" will never be reported as "works on the droid".

**Timing.** The controller ticks every millisecond. WiFi lives on the other CPU core, so it
should not interfere, but "should not" is not "does not". The firmware measures its own tick
timing and shows it on the Diagnostics page; if it is bad, we know immediately rather than
guessing. Saving settings to flash freezes the whole chip for tens of milliseconds while the
motors keep doing whatever they were last told, so the firmware refuses to save while the motors
are enabled and does it a moment later instead.

**Flash space.** v3 is at 94 percent of its allowed size. Dropping ReelTwo removes the biggest
consumer, and the new flash layout roughly doubles the room, so this should be comfortable.
We measure the size at every phase rather than assume.

**Upgrading.** A USB flash, a re-run of commissioning, and no automatic carry-over of settings.
A small tool will read a v3 `#PCONFIG` printout and replay the tunables and sequences into v4,
which softens this, but it is still a chore for every board.

**Mesh membership has strings attached.** Every WCB in the fleet needs firmware 6.2.0 or newer
or the periscope is invisible to it. The periscope's WiFi access point has to sit on the mesh's
channel, and the periscope cannot join your home WiFi while mesh is on. The library rewrites the
chip's MAC address. Broadcast messages are capped at 187 characters. None of this affects
builders who leave mesh off, which is the default.

**Sabé needs small changes to get the full benefit.** Showing the periscope's status on Sabé's
dashboard, broadcasting the E-STOP reset so the periscope releases together with Sabé, and
sending periscope commands directly to its device number instead of through a WCB serial port
are all separate work in the Sabé repo.

**Catalog and public work.** The DroidNet Command Library entry needs updating and a pull
request to Travis; that and anything pushed publicly waits for your say-so each time.

**iPhone WiFi quirk.** The fix that stops iPhones saying "Incorrect Password" lives in the
patched ReelTwo WiFi code we are throwing away, so it has to be rebuilt in the new WiFi code and
tested with your phone.

**Other builders' hardware.** Rotaries range from about 250 to 1,200 encoder ticks per turn
across builds, and lifters from 450 to 1,350 ticks of travel. The simulator has fixtures for
both ends, and commissioning measures each board rather than assuming, but real coverage of
other people's hardware only comes from beta testers. Plan on a beta release before anything
is called stable.

**Locking both motors until commissioned is deliberate, and it can surprise you.** A board that
loses its settings (factory reset, corrupted storage) will not move until it is re-commissioned
or bypassed. The bypass is on the Rescue page for exactly that moment at an event.

**Things that can bite during bring-up.** The first rotary command on a fresh board always
raises the lifter first, because the rotary's ticks-per-turn has to be measured above the dome.
The safe-spin height and the home-switch polarity have to be right before any rotation. Trigger
inputs wired to ground will fire their sequence as soon as the board boots.

**Size of the job.** This is many sessions. The motion core (stage 2) carries most of the risk
and gets the most tests; the web interface is the most visible and the least risky. Nothing is
event-ready before stage 5, which is why v3.6.0 stays flashable throughout.

## What I need from you along the way

- (Done 2026-09-07) The datasheet check on brake versus coast: both-low is a brake.
- Hardware testing at the end of each stage, following the written procedure, with the dome
  off for the first motion tests.
- A yes or no before anything public (pushes, pull requests, Discord posts).
- A decision on the heartbeat name: **`&PERI`** (Todd, 2026-10-04).

---

# Part 2 — Technical plan

Written 2026-09-07 against v3.6.0. This is the design and delivery plan for replacing the
patched-upstream firmware with a from-scratch codebase, the same way Amidala became Sabé and
Roam-A-Dome became Roam-A-Dome ALT. It is meant to be read at the start of every rewrite
session. The appendices carry the facts that took four exploration passes to establish, so
nothing here depends on the old code still being readable.

---

## 1. Context

**Why now.** v3 is Skelmir's upstream sketch with a year of patches on top: a 7,000-line
`.ino`, a 2,700-line `WebPages.h`, and a patched ReelTwo library that has to travel with it.
The September 2026 event failure (periscope raised, would only rotate, would not retract) cost
a full day of debugging and turned up defects that are structural, not local: two cores drive
the motors with no lock; every motion primitive blocks the core it runs on, so the web E-STOP
cannot be received during the wizard's own motion; four different definitions of "home" are
used at different sites; stored sequences have been broken past their first step since
v3.5.2; network OTA is registered but never started; the pulse loop performs two I2C
transactions every 4 ms; and the PWM carrier is 1 kHz, so a 3 ms pulse is three carrier
periods and its energy varies by a third with phase. Patching further is throwing good time
after bad. Sabé and Roam-A-Dome ALT proved the alternative works.

**Goal.** Same capabilities and the same Marcduino grammar, on the same community PCB, with:
no ReelTwo dependency; one motor authority; non-blocking motion so serial, web and E-STOP are
always live; a host-testable core with a mechanism simulator; a web UI designed around three
audiences instead of one page for everything; and first-class membership in Todd's droid
ecosystem (Sabé, the WCB mesh, DroidNet, the DroidNet Command Library).

**Non-negotiable.** The periscope is fragile, dangerous, expensive and a year of work. Every
design choice below is subordinate to: the head never descends into the dome unless the rotary
is home; the rotary never turns unless the head is clear of the dome; the lifter never slams
its limits; and any source can stop both motors within milliseconds.

### 1.1 Decisions made with Todd (2026-09-07)

| Decision | Choice |
|---|---|
| Where | Same repo `highfalutintodd/R2UppitySpinner_ALT`, new branch, ships as **v4.0.0**. v3 line frozen on tag `v3.6.0` plus a `v3-maintenance` branch. DroidNet board id `r2uppityspinner-alt` and the Releases URL stay valid. |
| WCB mesh | **Optional, default off.** Vendored WCBClient as its own device ID; receives `:P` over ESP-NOW; sends `&PERI` heartbeat/position/fault. Web AP pinned to the mesh channel when on; STA unavailable while mesh is on. |
| Dropped | SMQ Droid Remote + OLED menus, RC/PPM input, SD-card board variant, Marcduino-over-WiFi TCP 2000. |
| Kept regardless | Light kit (PCF8574 ABC), status NeoPixel, external trigger inputs A/B/C → sequences, WiFi AP + optional STA, web OTA. |
| Web UI look | **Sabé design system** (paper light default, slate dark opt-in, sticky header with state pill + E-STOP, left nav desktop / bottom tabs phone). |
| E-STOP | **Latch until cleared.** Release only by web button, serial `#PRESUME`, or mesh `&ESTOP,RESET`. Release lands in IDLE with motors disabled. `#PRESUME` stays in the `#P` namespace on purpose: stored sequences and chains can only carry `:P` elements, so a show can never release a stop. |
| First run | **Locked until commissioned.** Resumable checklist on-device; expert bypass on the same page with an explicit acknowledgement. Both motors locked, not just the lifter. |
| Operate page | **Buttons + presets.** Velocity controls (jog) live on Rescue. |
| Sequences | **Simple on-device editor** in the web UI emitting the `:P` grammar; the DroidNet composer is not vendored. |
| Build | Arduino + `sketch.yaml` profile (matches Sabé and RAD ALT; builders use Arduino IDE). PlatformIO and the reeltwo Makefile are retired. |
| Partition | `min_spiffs` (1.9 MB app × 2, OTA-capable). v3→v4 therefore requires one USB flash. |

---

## 2. What v4 must do

### 2.1 Preserved capabilities
- **Serial grammar**: every v3 `:P*` and `#P*` command (Appendix A) with identical parse rules, ID prefix (`:P<id>…`) filtering, and colon-chained commands. Fixes are additive: `:PD` supports multi-revolution and honours speed; stored sequences replay every step; `:PW` limits are consistent.
- **Lifter**: seek to % at speed; pulsed cruise with breakaway boost and soft approach; stall and slow-progress detection; three motor profiles (Appendix D) plus expert mode for the 19:1; invert; calibration; drift-correction hold; limit-switch polarity.
- **Rotary**: absolute, relative, continuous, home; runtime characterisation (`#PROTARYTEST`); min-power floor; safe-spin height; disable.
- **Safety**: descend-only-when-homed; rotate-only-when-raised; E-STOP from serial, web, mesh; driver-fault handling; jog leases; rescue override with countdown.
- **Shows**: random mode with six actions and Gentle/Medium/Aggressive (Appendix A.4), light kit modes 0–7, stored sequences 0–100, trigger inputs A/B/C.
- **Connectivity**: WiFi AP (`R2Uppity-XXXX` / `Astromech`, 192.168.4.1) with optional STA; web UI; web OTA; iOS-compatible softAP (WPA2-PSK, PMF not required: the ReelTwo PR #21 fix).
- **Diagnostics**: live state document, log, config dump, heartbeat, boot banner with version and profile.

### 2.2 New behaviour
- One motor authority (a control task); any context may stop, only the authority drives.
- Non-blocking motion state machines; commands from every source go through one queue.
- Boot position inferred from limit switches; unknown position permits up-only motion.
- Explicit home-confidence model (Appendix F.4) instead of four ad-hoc tests.
- Home switch is an edge detector while moving; homes are centre-referenced; a confirmed home is never abandoned.
- Rotary characterisation writes ticks/rev back when repeatable.
- Calibration measures what the seeks use: travel by repeatability, min power per direction, per-slot velocity and overshoot.
- Hardware pulse counting (PCNT) with a glitch filter for both encoders; 10 kHz PWM carrier inside the driver's 12–88 % pulse band (docs/TB9051FTG-NOTES.md).
- Per-source serial line assembly; per-line acceptance (`:`/`#` first byte; `?STOP`, `&ESTOP` recognised); cross-transport dedup.
- Logging with timestamps and levels into a RAM ring; edge events never throttled.
- E-STOP latch with one release path; commissioning lock; both visible in the state pill.
- Settings: versioned, append-only NVS blob with migrations; no flash writes while motors are enabled; `#PCONFIG` dumps replayable `#P<KEY>=<VALUE>` lines.
- Mesh: `&PERI` protocol (Appendix C.2); heartbeat period configurable; position/fault unicast to the special peer, heartbeat broadcast; silent on unknown verbs.
- Web: single `/api/state` polled at 2 Hz; every mutating call is POST and returns `{ok,err,msg}`; E-STOP endpoint unauthenticated and never rate-limited; assets gzipped in PROGMEM with `no-cache` + ETag.

### 2.3 Defects v4 closes by construction
Two-core motor race; blocking web handlers; wizard E-STOP unreachable; rescue override
cancelled by `rotaryMotorUpdate()`; four "home" definitions; failed home re-zeroing the
encoder; stored sequences broken past step one; `:PD` ≤180° and speed ignored; `ArduinoOTA`
never started; `#PFACTORY` not clearing calibration; `lftdist` inert; safe-spin % using two
denominators; fault chips rendered green; dead sequence tiles; `confirm()` dialogs as the
feedback system; 52 hex literals and 68 KB of uncached string literals; I2C enable writes
inside the pulse loop; 1 kHz carrier quantisation; dead calibration tables (the seek sets the
PID limit to 0 and never reads them); EEPROM persistence that may never have been flushed
(Appendix B.3).

---

## 3. Architecture

### 3.1 Repository layout (new tree on branch `v4`)

```
R2UppitySpinner_ALT/
  UppityFirmware/                 Arduino sketch folder (IDE-loadable, arduino-cli --profile uppity)
    UppityFirmware.ino            setup()/loop() only: wires modules together
    sketch.yaml                   pinned FQBN esp32:esp32:esp32:PartitionScheme=min_spiffs, core 3.3.x
    Board.h                       every pin (Appendix E)
    Version.h                     FIRMWARE_VERSION "4.0.0"
    src/core/                     PURE C++17, no Arduino headers, host-tested
      Hal.h                       abstract hardware interface (Appendix F.7)
      CircularMath.h Units.h      from Roam-A-Dome ALT verbatim; ticks/deg/% helpers
      Profiles.h                  LifterMotorProfile table (3) + RotaryCharacterisation struct
      Knowledge.h                 LifterKnowledge, RotaryKnowledge, HomeConfidence
      SafetyPolicy.h/.cpp         the invariants, evaluated in one place
      EstopLatch.h/.cpp           one-way latch, sources, release
      FaultModel.h/.cpp           faults, debouncers, latching rules
      LifterAxis.h/.cpp           Seek, Creep, Hold, Calibrate, bootResolve
      RotaryAxis.h/.cpp           MoveToTicks, Absolute, Relative, Spin, Jog, Home, Characterise
      Composites.h/.cpp           SafeLower, RaiseThenRotate, FirstUse
      MotionSupervisor.h/.cpp     the single motor authority
      CommandQueue.h StatusSnapshot.h EventLog.h
      Command.h Parser.cpp        Marcduino grammar → Command structs
      LineAssembler.h             per-source line buffer, acceptance, estop scanner
      Sequencer.h/.cpp            chains, stored sequences, :PW, random director, commissioning script
      Settings.h/.cpp             schema, defaults, ranges, sanitize, migrations
    src/sim/                      LifterModel, RotaryModel, SimHal (Appendix F.8)
    src/arduino/                  glue, Arduino-only
      Esp32Hal.cpp                LEDC 20 kHz, PCNT encoders, GPIO limits/DIAG, panicStop
      SupervisorTask.cpp          FreeRTOS task, core 1, priority 10, 1 kHz
      I2cService.cpp              sole owner of Wire/PCF8574: enable, light kit, trigger inputs
      SerialFront.cpp             USB console + command UART, onReceive → FIFOs, dispatcher
      Console.cpp                 #P handlers, #PCONFIG, #PSTATE, help
      Persist.cpp                 NVS writes only when disarmed; sequences; calibration blob
      Net.cpp                     WiFi AP/STA policy, mDNS, captive DNS (island only), OTA
      WebApi.cpp                  WebServer.h routes; one JSON writer with escaping
      WebAssets.h                 GENERATED gzipped PROGMEM assets
      Mesh.cpp                    WCBClient wrapper, device ID, special peer, &PERI protocol
      LogSink.cpp StatusLed.cpp LightKit.cpp
    libs/WCB_Client/              vendored, pinned tag (RAD ALT pattern)
    test/native/                  Makefile, uppity_test.h, Rig, test_*.cpp
  webui/                          index.html, app.js, uppity.css, api.js, mock-api.js
    build-assets.py               gzips webui/ into src/arduino/WebAssets.h
    dev-server.py                 serves the UI against a simulated device
  tools/                          release.sh (merged image + OTA bin), capture_config.py (v3 tunables)
  docs/                           ARCHITECTURE.md, COMMANDS.md, PROTOCOL.md, COMMISSIONING.md,
                                  INVENTORY-v3.md, UI-DESIGN.md, RELEASING.md
  CLAUDE.md, README.md, CHANGELOG.md, LICENSE (LGPL-2.1 unchanged)
```

Rules carried into the new `CLAUDE.md`: pure core vs Arduino glue (`#ifdef ARDUINO` only in
glue); definition of done = `make -C UppityFirmware/test/native test` +
`arduino-cli compile --profile uppity UppityFirmware`; tuning defaults have one source
(Settings struct → sanitize → parser table → `#PCONFIG` → COMMANDS.md, all or none);
settings append-only with migrations; everything the board emits must parse as a silent
no-op on every other board (`KEY=VALUE` lines, `&PERI,…`); RX callbacks flag or copy, nothing
else; only the supervisor task writes a non-zero PWM; only `I2cService` touches `Wire`; only
`Persist` touches NVS, and never while motors are enabled; say what you actually tested;
public actions need per-action approval; no runaway periscope.

### 3.2 Runtime: tasks, cores, priorities

| Context | Core | Prio | Does | Must not |
|---|---|---|---|---|
| supervisor task | 1 | 10 | 1 kHz tick via `vTaskDelayUntil`: read PCNT + GPIO, tick the active job, evaluate policy, write LEDC on change (brake tick between reversals), publish snapshot, push events, pet the task WDT (1 s) | I2C, NVS, Serial, heap, `String`, blocking |
| `loop()` | 1 | 1 | serial FIFOs → LineAssemblers → Parser → Sequencer → queue; I2cService; Persist; LogSink; StatusLed; `mesh.update()` | drive a motor |
| `net` task | 0 | 1 | `WebServer.handleClient()`, mDNS, OTA, captive DNS | call any motion primitive; block on the queue |
| UART `onReceive` | UART event task | 24 | estop scanner + push to the per-source FIFO | anything else |
| ESP-NOW RX callback | WiFi task | — | estop scanner + copy to a 16-deep queue | anything else |
| PCNT overflow ISR | 1 | — | accumulate | — |

Isolation is by priority preemption on core 1; the Arduino core runs `loop()` there too.
Stopping is allowed from every context: `EstopLatch::trip()` sets an atomic and calls
`panicStop()` (four LEDC zeros) directly; the supervisor observes the latch at the top of each
tick and again before each PWM write, so a drive can follow a trip by at most a few µs plus one
10 kHz carrier period. Driving is only ever done by the supervisor.

Motor enable (EN/ENB on the expander) is requested through an atomic and applied by
`I2cService`; a job that needs drive waits in `WaitEnable` (≤ 50 ms, else `EXPANDER_FAULT`)
and the enable is written only on change, so the pulse loop performs zero I2C. PWM zero is the
primary stop; EN low is the secondary and the idle power-down (2 s at rest). A WDT reset leaves
the expander powered up with ENB high, which disables the drivers: fail-safe.

### 3.3 Motion and safety core (summary; full spec in Appendix F)

- **Single authority**: `MotionSupervisor` owns both axes, one active job, the knowledge
  model, the policy, the latch and the fault model. Inputs: a fixed-size command queue plus
  out-of-band atomics (cancel, jog keepalive, override-until). Output: LEDC and a seqlocked
  `StatusSnapshot` for every reader.
- **Timing**: 1 ms tick; profile pulse on/off ms map 1:1 to ticks; LEDC 10 kHz / 10-bit (the driver needs ≥ 10 µs per PWM half, so 20 kHz would leave only 20–80 % usable);
  ESTOP under 1.1 ms worst case; `cycleUsMax` and `lateTicks` published so the claim is
  measurable.
- **Encoders**: PCNT hardware counters, ×1 decode (A positive edge, B level) so v3 tick scales
  survive, glitch filter 10 µs; GPIO-ISR quadrature is the fallback behind the same HAL.
- **Knowledge**: lifter `UNKNOWN/KNOWN/STALE`, rotary `UNKNOWN/SWITCH_LIVE/LATCHED/STALE`.
  A closed home switch at rest is trusted (the cam only conducts near centre); an open one
  carries no information and never demotes confidence.
- **Boot**: bottom closed → 0 known; top closed → travel known; neither → unknown, up-only;
  both → fault. Rotary switch closed → zero here. No motion at boot; the v3 safety maneuver
  is retired because composites cover every case on demand.
- **Homing**: pass 0 rest check; pass 1 encoder move to 10° before zero if a reference exists;
  pass 2 closed-loop creep sweep bounded by 1.25 rev (one reversal allowed), brake on the edge,
  zero at the centre of the contact arc; pass 3 best-effort settle that can never change the
  outcome. Failure leaves the frame untouched.
- **Interlocks** (one function): rotary refused below safe height; descent across safe height
  needs `LATCHED` or `SWITCH_LIVE` (or override), and once a creep has stopped a turned head at
  that height (`clampHeld`) every further descent is refused until a home, a 20-tick rise or a
  new safe-spin setting; lifter unknown ⇒ no descent, no override;
  E-STOP or commissioning lock ⇒ both zero; DIAG ⇒ that axis zero; lease expired ⇒ zero.
  Refusals return coded reasons, and a clamped seek reports `CLAMPED(reason)` loudly.
- **E-STOP**: latch from any source; cancels job, queue, chain, sequence, random mode and the
  commissioning script; release by `#PRESUME`, web reset, or mesh `&ESTOP,RESET`, landing in
  IDLE with nothing queued. Web `/api/stop` is a separate non-latching soft stop. The latch
  brakes (both PWM low) and keeps the drivers ENABLED until both counts have been still for
  100 ms (2 s at most), then disables them: the short brake needs EN high, and a lead screw
  that back-drives under the mast's weight free-wheels with the outputs floating (bench
  2026-10-03). `ESTOP braked to a stop: …` logs how far each axis went after the latch.
- **Faults** (Appendix F.6): DIAG with EN-cycle recovery, repeated DIAG latching, stall,
  slow progress, encoder direction, phantom ticks, limit contradiction, limit mismatch, stuck
  home switch, expander fault, tick overrun, WDT, persist-while-armed (deferred, never lost).

### 3.4 Command layer
- **Sources**: USB console, command UART, web `/api/cmd`, mesh, sequencer, random director,
  trigger inputs. Each serial source has its own 256-byte `LineAssembler` and dispatcher state.
- **Acceptance per line**: first byte `:` or `#` → ours; `?STOP`, `&ESTOP…`, `&SABE,ESTOP…`
  → latch; everything else (heartbeats, WDP chatter, other boards) dropped whole, never logged
  as "Invalid". ID mismatch drops silently.
- **Dedup**: identical lines from different transports within `dedupMs` (750) execute once.
- **Parser (pure)**: the v3 grammar to `Command` structs. `#PS` accepts both the legacy element
  form (`P100:L5:R30`) and the full `:PP100:PL5:PR30` form and stores the canonical `:P` form,
  so store and replay grammars are identical; validates without executing; `:PW` clamp 60 s.
- **Sequencer (pure, non-blocking)**: a chain is a list of commands; blocking ones wait for the
  job result; `:PW` is a timed state; `:PL` and `:PW` do not end random mode; other `:P` motion
  commands do; E-STOP clears everything. The random director and the commissioning script are
  sequencer scripts, so policy applies to every job they issue.
- **Output discipline**: responses are `KEY=VALUE` or `&PERI,…` lines; `#PCONFIG` emits
  replayable `#P<KEY>=<VALUE>` lines.

### 3.5 Settings and persistence
One `Settings` struct (Appendix B.2) in NVS namespace `uppity`, `schemaVersion` stamped, grown
only at the end, prefix-copy migration, `sanitize()` clamps on load, save skipped when
unchanged, and **written only through `Persist` when the snapshot says disarmed** (a flash
erase stalls both cores while LEDC keeps emitting; `persistBusy` blocks arming during the
commit). Requests that arrive while armed are deferred and logged, never dropped. Sequences are
NVS strings `seq00..seq99` plus names. Calibration lives in its own blob so re-commissioning
does not touch WiFi/mesh settings. `#PFACTORY` clears all three and reboots; `#PZERO` clears
sequences only. OTA start is refused while armed.

### 3.6 Networking
- **WiFi**: AP always up first (`R2Uppity-XXXX` / `Astromech`; WPA2-PSK, PMF off); STA optional
  with Sabé's backoff and "no scan while a client is on the AP" rule; credentials stored then
  applied by reboot; captive DNS only while STA is down; mDNS `uppity.local` registered after
  OTA begin. AP channel = mesh channel when mesh is enabled; STA refused while mesh is on.
- **Web server**: core `WebServer.h` in the `net` task; gzipped PROGMEM assets with `no-cache`
  + ETag keyed to an asset hash; JSON through one escaping writer; all mutations POST;
  `/api/estop` exempt from any auth or rate limit. A device PIN for mutating routes is backlog.
- **OTA**: web upload (`POST /update`, app-only `.bin`) with progress; `ArduinoOTA` started for
  the IDE network port; both refused while motors are enabled.
- **Mesh**: `WCB_Client(oct2, oct3, password, quantity, deviceId, cb, statusCb)`,
  `setMeshChannel`, `setChecksum`, `setIdentity("Uppity Spinner", fw, "uppity-pcb", "periscope,lift,rot,seq,lights")`,
  `enableSpecialPeer(20)`, `WiFi.setSleep(false)`; refuses to start without a password.
  Inbound `:P`/`#P` lines go to the command queue, E-STOP triggers to the latch. Outbound per
  Appendix C.2.
- **DroidNet**: nothing to register; the Signal Booster taps everything the WCB forwards. The
  board file (Appendix C.4) is regenerated for 4.0 and offered upstream with approval.

### 3.7 Logging and diagnostics
`EventLog` (SPSC ring of coded events, formatted outside the tick) plus a 16 KB text ring with
ms timestamps and levels; tee to the USB console; `/api/log?since=`. Edge events (home switch,
limits, faults, state and confidence changes, clamps) are logged unconditionally. Verbose
motion tracing behind `#PDEBUG1`. Boot banner: version, build date, profile, commissioning
state, settings version, boot resolution line. `#PSTATE` prints the snapshot; `#PCONFIG` the
replayable settings; `#PROTARYTEST` the characterisation report.

### 3.8 Status LED and light kit
NeoPixel: IDLE-home solid green, IDLE-away solid red, MOVING green/yellow, STOPPED (latched)
fast red blink, FAULT red/yellow, LOCKED (uncommissioned) solid yellow, WiFi-client variants in
blue as today. Light kit truth table unchanged (Appendix A.5).

---

## 4. Web UI design

Design system: Sabé's tokens verbatim (`--bg --raise --raise2 --well --field --text --accent
--accent-hi --ok --warn --bad`, values in Appendix C.5), `html{font-size:20px}` and rem units,
system font stack (Schibsted Grotesk when online), 44 px minimum targets, WCAG AA contrast on
every label. Theme toggle in the header; default paper; persisted in `localStorage` and in
`Settings.theme` so a new phone gets the owner's choice.

**Shell.** Sticky header: wordmark, state pill (`LOCKED · IDLE · MOVING · STOPPED · FAULT ·
NO LINK`, dashed amber for NO LINK), height and rotation readouts, E-STOP button (solid red,
on every page, fixed on phones). Desktop: left nav. Phone: five bottom tabs. `GET /api/state`
polled at 2 Hz and diffed; every mutation returns `{ok,err,msg}` shown inline on the control
plus a toast; error codes glossed in one `ERR_TEXT` map; no `alert()`/`confirm()`; busy states
disable controls with a timeout. Sticky banners: STOPPED (with reset), OVERRIDE ACTIVE
(countdown + cancel), LOCKED (link to commissioning), UPDATE IN PROGRESS.

| Page | Audience | Contents |
|---|---|---|
| **Operate** | event operator | Lifter: Up, Down (safe lower), presets 25/50/75/100, speed segmented control (Gentle/Normal/Fast mapped through the profile). Rotary: Home, 0/90/180/270, Spin L/R with Stop, speed. Show: Random Gentle/Medium/Aggressive + Stop. Lights: eight pills. Sequences: tiles for occupied slots only, with names. Soft Stop. Nothing diagnostic. |
| **Sequences** | operator/builder | Slots with names; editor with a step composer (lift to %, rotate to °, relative turn, spin, wait, lights, random, home) showing the resulting `:P` text; Test, Save, Delete; import/export as text; trigger inputs A/B/C mapping. |
| **Setup** | builder on the bench | Commissioning checklist; Motor & calibration (profile, invert, run calibration with live progress, expert mode with its warning card); Tuning (min powers, safe-spin height with "use current", drift %, aggressiveness, rotary disable); Serial (ID, baud); WiFi; Mesh (enable, IDs, channel, password, heartbeat period); Firmware (version, OTA upload with progress, releases link, reboot, factory reset); Theme. |
| **Diagnostics** | builder | Live sensors (limits, encoders, DIAG, confidence, leases, tick stats); Rotary characterisation runner with the report; Log (filter, pause, download); Config dump (replayable text); Mesh neighbours and last heartbeat. |
| **Rescue** | emergency | E-STOP / Reset; hold-3-s Safety Override with 60 s countdown; hold-to-jog Up/Down (continuous, capped to profile) and rotary L/R (10% floor, override only); Set rotary home here; Clear fault; Unlock (commissioning bypass with acknowledgement). Every destructive action is a two-step inline confirm. |

**Commissioning (Setup → Commissioning).** State on the device (`commissionState`), resumable
across reloads, E-STOP live throughout because nothing blocks. Steps: 1 Profile (select + the
five acknowledgements) → 2 Wiring check (live limit/encoder/DIAG strip, instructions, ack) →
3 Creep down then up (continuous drive, stops on limit; both must pass) → 4 Lifter calibration
(progress from the snapshot) → 5 Rotary characterisation (raises to safe height, runs
`Characterise(3)`, shows ticks/rev, arc, chatter, verdict; fails loudly with a polarity hint if
the switch never fires) → 6 Acceptance (0/50/100, home, one random action) → Complete.
Changing the motor profile anywhere resets to step 1; there is exactly one bypass and it is on
this page.

**Browser verification.** `webui/dev-server.py` simulates the device (state machine, refusal
codes, commissioning) so every page can be exercised in the in-app browser before any flash;
`mock-api.js` backs static viewing.

---

## 5. Delivery phases

Each phase ends with: host tests pass, `arduino-cli compile --profile uppity` clean with the
size line recorded, and a written bench procedure for Todd. v3.6.0 stays flashable throughout.

| Phase | Deliverable | Verification |
|---|---|---|
| **0 Skeleton** | Tree from §3.1, `sketch.yaml`, `Board.h`, `Version.h`, `CLAUDE.md`, `docs/INVENTORY-v3.md` (Appendices A–F and the four exploration reports), `Hal.h`, `Sim`, test harness, empty modules that compile | `make test` runs; compile clean; size baseline. Todd: confirm the TB9051FTG brake/coast truth table from the datasheet (Appendix F.9) |
| **1 Motion core + console** | `Esp32Hal` (PCNT, LEDC 20 kHz, limits, DIAG, expander enable via I2cService), supervisor task, Lifter/Rotary primitives, composites, policy, latch, faults, knowledge, snapshot, event log, USB console with `:P`/`#P` motion commands, `#PSTATE`, `#PROTARYTEST`. No WiFi. | Sim tests (§6); bench: boot resolve from up/down/unknown, seek 0↔100 at three speeds, calibration, home from 12 starting angles, `:PX` mid-seek then `#PRESUME`, DIAG fault, `cycleUsMax`/`lateTicks` with the dome on |
| **2 Command layer + settings** | Command UART, per-source assemblers, dedup, Sequencer, `#PS` store/replay, random director, light kit, trigger inputs, status LED, Settings/Persist with migrations, `#PCONFIG`, `#PFACTORY` | Parser/sequencer tests; bench: README chains, sequences 0–8, random soak at all levels, SABE/RAD heartbeats on the bus, power-cycle persistence, NVS deferral mid-seek |
| **3 Network + minimal UI** | `Net.cpp` (AP/STA, mDNS, captive, OTA both paths), `WebApi.cpp` (`/api/state`, `/api/cmd`, `/api/estop`, `/api/estop/reset`, `/api/stop`, `/api/config`, `/api/log`), asset pipeline, shell + Operate page | Dev-server check; bench: iPhone joins AP, Operate page drives the periscope, E-STOP during a seek, OTA of the next build |
| **4 Full UI** | Sequences editor, Setup (commissioning, calibration, tuning, wifi, firmware, theme), Diagnostics, Rescue (override, jog, unlock) | Commissioning from factory reset on hardware; rescue override honoured for the full 60 s; dark theme review |
| **5 Mesh + DroidNet** | `Mesh.cpp` with vendored WCB_Client, `&PERI` protocol, mesh E-STOP, dedup across serial + mesh, Mesh settings page; regenerate `r2uppityspinner-alt.json` for 4.0 | Bench with Sabé + WCB fleet: `:PP100` over mesh, `&PERI,HB` visible in the DroidNet console, `?STOP` latches, `&ESTOP,RESET` releases; PR to Travis with approval |
| **6 Release** | README (builder voice, migration from v3, behaviour changes), CHANGELOG, `docs/`, merged flash image + app-only OTA bin via `tools/release.sh`, `capture_config.py` for v3 tunables, GitHub pre-release `v4.0.0-beta.1` | Fresh-board flash from the merged image; upgrade path documented; event soak |

Order inside Phase 1: Hal + Sim + tests; bootResolve and SafetyPolicy before any seek; Seek;
E-STOP; Rotary spin; Home; Characterise; Calibrate last.

**Status 2026-09-20:** Phases 0 and 1 done and bench-verified on Todd's periscope (branch
`v4-rewrite`; results and the per-boot settings in `docs/BENCH-PHASE1.md`). Two design changes
came out of the bench and supersede Appendix F where they differ: (1) the safe-spin default is
the top, never half travel; (2) homing pass 3 is a switch-only park (back off clear of the arc,
return at floor power, brake on the first closure while moving) because the encoder on Todd's
drive cannot be trusted tick by tick after the frame strike; the arc-centre-by-count park
remains as `#PHOMEPARK0`.

**Status 2026-09-20 (later):** Phase 2 implemented on `v4-rewrite`: `Settings`/`Calibration`
blobs in NVS namespace `uppity` (append-only, prefix-copy migration, written only while
disarmed with the supervisor's intake held), one-shot conservative import of v3's
`uppityspinner` tunables, the KEY=VALUE registry behind `#P<KEY>[=]<value>` and a replayable
`#PCONFIG`, the command UART with its own line assembler, cross-transport dedup, the
non-blocking sequencer (`:PW` no longer blocks), stored sequences in namespace `uppityseq`
(legacy and full forms accepted, canonical `:P` form stored), the random director as a
script generator, `:PL0` restart pulse, debounced trigger inputs (baseline at boot, never a
power-up fire), status LED module. 106 host tests; ESP32 build 407,511 bytes (20 %). Bench
procedure `docs/BENCH-PHASE2.md`; hardware verification pending. Deviations from §3.5/A.2:
sequences number 0–100 as v3 did; `#PMOTOR` to a *different* profile resets commissioning,
travel, safe-spin and min powers (same profile is a no-op); the builder's `#PTRAVEL` wins over
the calibrated travel (0 hands back to the calibration); `#PS` waits clamp to 60 s at store time.
Bench 2026-09-20 evening (Todd): passed except stored sequences / command UART / triggers, not
run; four log findings fixed the same night (nearest-zero home after a spin, first-tick late
artefact and TICK spam, director homes only if needed, full-travel already-there report) and
`#PRANDOMLIGHT` added (settings schema 2). Results and the re-test list: `docs/BENCH-PHASE2.md`.

**Status 2026-09-20 (night): Phase 3 implemented**, awaiting the bench: `Net` (AP always up,
`R2Uppity-XXXX`/`Astromech`; optional STA with Sabé's backoff and no retry while a phone is on
the AP; captive DNS while STA is down; mDNS `uppity.local` after ArduinoOTA; web server in its
own task on core 0), JSON API (`/api/state`, `/api/cmd`, `/api/estop` any method, `/api/estop/reset`,
`/api/stop`, `/api/config` GET/POST, `/api/log?since=`, `/api/restart`, `POST /update`), one
gzipped page (Operate + console; Sabé tokens, paper default, dark opt-in), the 16 KB log ring
fed by every console line, `#PSSID/#PPASS/#PAPPASS/#PWIFI/#PAPCHANNEL/#PTHEME/#PWIFIRESET`
(settings schema 3), `webui/build-assets.py` + `dev-server.py`. Deferred to Phase 4: the
Sequences/Setup/Diagnostics/Rescue pages, a device PIN, a UI for the update upload. Build
1,135,007 bytes (57 %). Bench: `docs/BENCH-PHASE3.md`.

**Status 2026-09-27 (evening): Phase 3 bench-verified** after four runs; the fixes (port-81
E-STOP listener, four-channel retrying E-STOP button, 400 ms stall guard, poll timeouts, seek
interruption with a post-interrupt rest gate, ELF-hash build id) are recorded in that doc.
Phase 4 starts from `docs/PHASE4-BRIEF.md`.

**Status 2026-09-27 (night): Phase 4 implemented**, awaiting the bench (`docs/BENCH-PHASE4.md`).
One page file with four pages (Operate, Setup, Diagnostics, Rescue; bottom tabs on a phone, a left
column on a laptop), the sticky banners, and the Phase 3 E-STOP untouched. Setup: every settings key
as a form field whose "saved" comes from the board's own `#PKEY=… (saved)` line; WiFi; firmware
upload with progress; restart; factory reset behind a confirm panel; theme. Diagnostics: live
sensors, the rotary test with its report lines, the log (filter, pause, download, command box),
the `#PCONFIG` text with copy/download and a line-by-line restore. Rescue: the override
(`#POVERRIDE`, 3 s hold, 60 s countdown, accepted only from USB or the page), hold-to-jog through
`POST /api/jog` (renewed every 500 ms, 2 s lease, release on every way a finger can leave),
`#PSETHOME`, clear fault. The only new endpoints are `/api/jog` and `/api/jog/stop`; everything
else is a `#P` line. Commissioning is a pure `CommissionFlow` whose step persists in
`Settings.commissionStep` (schema 4) and which decides every pass from the supervisor's results.
Deviations from §4, from the bench history and the v4 policy: (1) the steps run profile, wiring,
**lift test**, rotary, calibration, **safe-spin (measured)**, acceptance; the lift test creeps
**up** only, because with the rotary not yet homed the policy refuses a descent from the top, and
the way down is proven by the calibration's first move; (2) the safe-spin height is measured: RUN
clears it (only the top counts), raises and turns the head 90° at the top, the operator lowers it
in bounded `#PNUDGE` steps under the override, and `#PSAFESPINHERE` stores the height plus 3 % of
travel (at least 20 ticks), accepted only in that step with the head at least 45° off home and
below the top; (3) the one bypass is on the commissioning page (`#PCOMMISSIONBYPASS`, the same as
`#PUNLOCK`), and Rescue links there. Safety changes that came with the override: the top switch is
never "inside the opening" (a turned head at exactly the travel count could have been let down
with no safe-spin set); after the override lets the head descend unaligned or jog below the safe
height, `alignmentSuspect` refuses further descents until a home; every creep stops when the
switch it is leaving closes again (`WRONG_LIMIT`, a reversed mechanism); the E-STOP ends the
override and the jog lease. 139 host tests (the full seven-step commissioning runs on the
simulator), ESP32 build 1,173,871 bytes (59 %).

**Status 2026-10-03: Phase 4 bench run 1 — the head hit the lifter frame.** With the safe-spin
height raised to 1100 and the head turned 90° at the top, a Rescue jog Down stopped at 1100 as
designed, and the next press was let through. Root cause in `SafetyPolicy`: the rule-2 exemption
("at or below the safe-spin height = inside the opening = lined up") treated a turned head that the
clamp had parked exactly at the threshold as inside the opening. The hole dated from Phase 1 (two
`:PP` presets in a row with the head turned would do the same); the jog made it easy to reach, and
jogs had no stall exit, so it kept pushing. The exemption is now conditional: the supervisor keeps
`turnedSinceHome_` (set whenever the head is off home above the safe height, or turns below it under
the override; cleared only by a home), and every descent below the safe height is refused while it
is set; SafeLower raises, homes, then lowers from that state; the boot case (a head parked inside the
dome at power-on) is unchanged. Jogs (lifter and rotary) now stop on a stall; parameterised random
mode homes before its random lifts; the page's E-STOP no longer moves under a mouse press. The bench
procedure's step 13 relied on the refusal next to an unverified clearance; CLAUDE.md hard rule 12 now
forbids that. 144 host tests; 1,174,219 bytes.

**Status 2026-10-03 (evening): re-test of that fix passed; E-STOP coast found and fixed.** On the
droid, a turned head at the 1100 threshold was refused by `#PNUDGE` and `:PP94` with nothing moving
(the first nudge, started above 1100, clamped there and came to rest 12 ticks below it: braking
distance at 15 %). The same log showed an E-STOP during a descent letting the mast run on to the
bottom switch (121 ms and 505 ms after the latch, the second from about 600 ticks): the E-STOP
branch disabled the drivers on its first tick, and with EN low the TB9051FTG floats the outputs, so
the short brake stopped working and the back-driving screw coasted. Ordinary stops keep EN on for
`idleDisableMs` and never showed it; the simulator modelled the screw as self-locking. Fixed: the
latch keeps the drivers on, braking, until both counts are still for 100 ms (2 s at most) and logs
`ESTOP braked to a stop: lifter ±N, rotary ±N since the latch; drivers off after N ms`; the
simulator gained a back-drive model (holds at rest, falls on when floating), and the bench sequence
is a host test that fails on the old build. The alignment log line no longer claims "the head is
home" when the mast merely rose above the safe height. Open: the creep's clamp at the safe height
lands a braking distance below it (12 ticks at 15 % on Todd's lifter); brake early by the measured
distance once re-test 2 has logged it at full speed. 151 host tests; 1,174,727 bytes.

**Status 2026-10-04: E-STOP fix verified on the droid; creeps brake early.** Re-test 2 (build
`db08a868`): four E-STOPs during full-speed descents stopped within 53–60 ticks and two during rises
within 31–32, drivers off 155–282 ms after the latch, nothing ran on to a switch. Todd's lifter
brakes in about speed × 40 ms going down and × 27 ms going up, the same proportion as the 15 %
creep's 12 ticks. `LifterCreep` now starts braking `ahead` ticks early for a step (`#PNUDGE`) and
for the policy floor: its speed over the last 16 ms times `brakeLeadMs`, the median of the
calibration's `over / vel` per slot (the stopping distances the seeks already use; capped at
150 ms; 0 without a calibration, which keeps the old behaviour); the floor adds a quarter and a
tick. A creep the floor stopped sets `clampHeld` (refuse descents until a home, a
`kClampRearmTicks` rise or a new safe-spin height), because a second press from rest just above the
height cannot brake early enough. The log reports `NUDGE came to rest …` / `CLAMP came to rest …`
once the counts are still, and `#PCAL` prints `CALBRAKE`. 155 host tests; 1,175,979 bytes.

**Status 2026-10-04 (later): Part D (commissioning) ran with the dome off; two measurement fixes.**
All seven steps passed on build `db08a868` and the board is `COMMISSIONED=2`; the safe-spin value
(897) was measured without the dome and stands in only until a dome-on run. `RotaryCharacterise`
counted the switch the head starts parked on as an edge, so the first revolution ran from the park
spot (1179 + 1185 + 1185 → 1183.0 stored for a 1185.5 rotary); it now ignores the starting closure
and counts only edges it drives into. `LifterCalibrate` P1 measured travel between the resting
spots (1192), but the frame is referenced at the switch closings (bottom closing = 0, top closing =
travel; ~1138 apart on Todd's lifter, which runs on 25–30 ticks past each switch while braking), so
the calibrated travel made the count jump at every switch; P1 now uses the closing positions the
creeps report (the first up-run only if the first creep really closed the bottom switch), and the
calibration ends with 0 at the bottom switch's closing point. 157 host tests; 1,176,139 bytes.

**Status 2026-10-04 (evening): Phase 4 bench-passed.** Re-test 3 (build `b14aa631`, installed
through the page's own firmware upload): `#PROTARYTEST` stored 1185.7 ticks/rev; `CALBRAKE` 32 ms
down, 20 ms up (the table's braking time grows with speed: 27→38 ms down, 5→28 ms up); nudges came
to rest at −21/−51/−13/+16 for −20/−50/−12/+20; a turned head jogged toward 1100 came to rest at
+4 and the next descent was refused until a home. The operator's rotary test now homes afterwards
(it stopped ~11° past home, outside the 10° window). Open: the dome-on commissioning run (Todd's
real safe-spin height; the first hardware check of the switch-closing travel), Part E step 32 and
Part F (optional). Phase 5 needs Todd's heartbeat-tag choice (`&PERI` or `&UPPITY`). 158 host
tests; 1,176,195 bytes.

**Status 2026-10-04 (night): dome-on commissioning done; heartbeat tag `&PERI`.** On build
`77cbfb02`, all seven steps with the dome on: rotary 1185.0 ticks/rev, travel 1137 between the switch
closings (the fix confirmed: after the calibration the top switch closed at 1135 in a frame with 0
at the bottom closing), safe-spin **878** (stopped at 844 + 34). It also showed the last reference
that used a resting spot: `bootResolve` set 0 where the mast rests on the bottom switch, ~33 below
the closing point, so after power-up the height read ~33 high until a switch edge (the lift test saw
the top close at 1172). `LifterCalibrate` now records the deepest rest below the bottom closing
(`bottomRest`, calibration schema 2, carved from the reserve) and power-up on the bottom switch
starts the count at minus that (`MotionConfig.bootBottomTicks`): it may read a little low after a
shallower rest, never high. Todd sends `#PTRAVEL0` (1137 agrees with both switches). 161 host
tests; 1,176,647 bytes. Next: re-test 4 (recalibrate once to measure the rest, restart at the
bottom, check the top closing), then Phase 5.

**Status 2026-10-04 (late): Phase 4 complete.** Re-test 4 (build `9743ad66`): the calibration stored
`rest=37`, power-up on the bottom switch started the count at −37, and the top switch then closed at
1131 for 1137 of travel (it was 1172 that morning); `#PTRAVEL0` is in use. Phase 5 starts from
`docs/PHASE5-BRIEF.md`.

**Later the same day:** Todd's free testing (random at all levels, spins, Comlink chains, `:PH`
interrupting spins and random) found a window in `rotaryIsHome`: SwitchLive follows the switch only
after a 400 ms hold at rest (and not at all while the rotary is busy), so a head that coasted off the
switch after a spin still counted as home and a descent queued behind the spin could start (one coasted
13° past home; Todd's `:PH` came 618 ms later and homed first). SwitchLive now counts as home only while
the switch reads closed; off it the count decides, as for Latched. 163 host tests; 1,176,727 bytes.

**Status 2026-10-04 (late night): Phase 5 implemented**, awaiting the bench (`docs/BENCH-PHASE5.md`,
branch `claude/phase5-mesh`). Part A, the code review's D5: a seek brakes `ahead` ticks before its
target, its speed over the last 16 ms (carried forward by a quarter of the change between the two
8 ms halves while it is clearly accelerating or slowing) times the braking time the calibration
table gives at that speed (`BrakeTable`, each slot held within half and twice the median); short
steps always run the breakaway burst (pulsed power from rest starves a 19:1); a run to a switch is
unchanged. Every descent the policy gives a floor (the creep's floor: the head may be turned)
brakes against it on the safe side (higher estimate, the longer time, a quarter and a tick; 60 ms
with no table), and from close above it is held where it is (≤ 20 ticks) or goes the slow way, as
a creep (≤ 60 ticks); a seek stopped at the floor holds the clamp. On the simulator (a copy of
Todd's 19:1 with its own calibration) steps of 10-400 ticks land within 9 ticks (mean 2.5); before,
nothing under ~60 ticks moved. An independent review of the first version found a turned head sent
to a target just *above* the floor could overshoot below it; fixed and tested. The mesh (§1.1):
off by default; WCB_Client 1.17.1 vendored in `libs/WCB_Client`; `MeshProtocol.h` decides (pure)
and `Mesh.cpp` moves bytes; the radio task only trips the latch and copies the line; mesh `:P` is
unattended (`ReqSource::Mesh`), mesh `#P` is limited to questions and `#PRESUME`; `&PERI` per C.2
with HB/POS/STATE/FAULT/ACK/HELLO; mesh on = AP on the mesh channel, no home network; settings
schema 5. Todd's device id is 5 with a WCB quantity of 3: the WCBs must learn it from its WDP
adverts (bench step 13 checks). 232 host tests; 1,224,987 bytes (62 %).

---

## 6. Verification strategy

**Host tests** (`test/native`, C++17, `-fsanitize=address,undefined`, RAD ALT harness). The
simulator (Appendix F.8) models static friction, momentum, self-locking, limit windows,
starvation under pulsing, phantom ticks, DIAG, frozen encoders, stick-slip rotary bursts, and
the spherical-cam switch with chatter and direction-dependent first contact. Tests that must
pass before Phase 1 touches hardware, grouped by the failure they would have caught:

- *September homing failure / parks at 66%*: home finds the edge from 13 start angles on both
  250 and 1191 tick fixtures and a fixed 3/1 pulse fails the same rig; search bound is
  distance not time; CW and CCW homes agree within 2 ticks; chatter yields one edge event per
  pass; failed home never re-zeroes; a resting open read does not block descent; an unhomed
  droid stops at safe height and says why; settle failure keeps the home; a rest-contact read
  corrects a wrong frame loudly.
- *Boot at zero*: mast-up boot references from the top limit with no motion; mid-travel boot
  permits only up until referenced; both limits closed is a fault.
- *E-STOP*: trip from a second context mid-calibration zeroes PWM within two ticks and the
  supervisor never re-drives; latch flushes chain, `:PW`, random mode; `:PP` refused until
  `#PRESUME`, `:PL` still works; the RX scanner catches `:PX` mid-chain.
- *Rescue override race*: rotary jog under override below safe height runs through 100 policy
  evaluations and stops only at its lease; floors are 10% web / 40% serial.
- *`:PD`*: ±720 turns two revolutions; speed honoured; `:PA` after `:PD720` lands in tolerance.
- *Mechanism*: cruise pulses and limit impact velocity is bounded (continuous drive exceeds
  it); creep is continuous and moves the 19:1 fixture; breakaway → ramp → one re-boost; stall
  timeouts follow the profile; travel accepted by repeatability not magnitude; characterise
  writes back at ≤ 5% spread only; DIAG recovers after an EN cycle and the calibration wait is
  bounded at 15 s; NVS write mid-seek is deferred; the tick makes no I2C calls; enable is
  written only on change; the seqlock never yields a torn read; three home passes at 2 s
  spacing produce three distinct events.
- *Command layer*: `P100:L5:R30` replays all three steps; `:PP100:W2:P0` completes; SABE/RAD
  heartbeats mid-command never truncate a console line; same command on serial and mesh within
  750 ms executes once; settings v1→v2 migration keeps every field and a short blob loads
  defaults without overwriting NVS.

**Compile.** `arduino-cli compile --profile uppity UppityFirmware` with a build path under the
scratchpad; record the size line per phase.

**Browser.** `webui/dev-server.py` + the in-app browser for every page in both themes at phone
and desktop widths.

**Hardware (Todd).** Written procedures per phase; "compiles clean, host tests pass, verified
on the owner's hardware" is the only allowed wording; untested paths are named as such.

---

## 7. Risks and open items

- **TB9051FTG output truth table: RESOLVED 2026-09-07** (datasheet in `docs/datasheets/`,
  notes in `docs/TB9051FTG-NOTES.md`). PWM1=PWM2=low is a *short brake* (both low-side FETs
  on; both-high is the same state); High-Z coast only with EN low or ENB high. So v3's pulse
  "off" period has always been a brake, which is why pulsing decelerates the screw so well.
  Two consequences carried into the design: forward and reverse must pass through brake (the
  datasheet says the IC may be damaged otherwise), and DIAG reads low whenever the drivers are
  disabled, so it only means "fault" while enable is acknowledged; over-current and
  over-temperature latch DIAG low until an EN rising edge.
- **PCNT on core 3.3.x.** `driver/pulse_cnt.h` is present; the legacy `driver/pcnt.h` cannot
  coexist, so no `ESP32Encoder` library. Fallback is GPIO-ISR quadrature behind the HAL.
- **10 kHz carrier moves the min-power floors** (v3 ran 1 kHz). Commissioning re-discovers
  them; profile defaults are starting points only. 20 kHz was the first choice but the driver's
  10 µs minimum pulse leaves only 20–80 % usable there. Whine at 10 kHz is the accepted trade.
- **Flash budget.** Removing ReelTwo removes the largest consumer; measure at Phase 0 and every
  phase; `min_spiffs` gives 1.9 MB per slot.
- **v3 state does not migrate.** v3's EEPROM-backed calibration may never have persisted
  (Appendix B.3); every board starts at commissioning. `capture_config.py` carries the NVS
  tunables and sequences from a v3 `#PCONFIG`/`#PL` dump.
- **Behaviour changes to tell builders**: E-STOP latches and needs `#PRESUME`; `:PH` no longer
  loops raise-home-lower on failure (it stops raised and says so); `:PD` really rotates;
  calibration must be re-run; no safety maneuver on first motion.
- **Mesh constraints**: WCB firmware ≥ 6.2.0 on the fleet or device-20 traffic is invisible;
  broadcast ≤ 187 chars with checksum; STA off while mesh is on; the factory MAC is overwritten.
- **Sabé-side follow-ups** (separate repo, later, with approval): show `&PERI` HB/POS on the
  dashboard; broadcast `&ESTOP,RESET` on its own reset so the periscope releases with it; route
  `:P` commands to the periscope device ID instead of `;W3;S4`.
- **DCL upstream**: target the current schema (`category`, not `group`); bump `libraryVersion`
  minor; `npm run validate` + `npm test`; PR only with approval.
- **Naming**: heartbeat tag **`&PERI`**, decided by Todd on 2026-10-04 (`&UPPITY` was the
  alternative).
- **iOS softAP**: replicate the WPA2-PSK / PMF-off fix from ReelTwo PR #21 in `Net.cpp`.

---

## Appendix A — Serial grammar (v4 keeps all of v3; deltas marked ★)

### A.1 Motion `:P…` (optional decimal ID after `:P`; chained with `:`)
| Command | Behaviour |
|---|---|
| `:PP<0-100>[,speed]` / `:PPR[,speed]` | Seek to % (random with `R`); speed default 100, floored at profile min; a clamp is reported |
| `:PH[speed]` | Safe lower: raise if unknown → home if needed → lights off → lower to 0 ★ no retry loop |
| `:PA<deg>[,spd][,max]` / `:PAR` | Absolute rotation, shortest path; homes/characterises first if needed; `0` = home |
| `:PD<deg>[,spd][,max]` / `:PDR` | Relative rotation ★ multi-revolution, speed honoured |
| `:PR<±speed>` / `:PRR[max]` | Continuous spin; `0` = home |
| `:PM` / `:PMG` `:PMM` `:PMA` / `:PM[,lift][,rot][,min][,max]` | Random mode: default level / one-shot level / parameterised |
| `:PW[R]<sec>` | Wait (random 1..N with `R`), clamp 60 s ★ everywhere |
| `:PL<0-7>` / `:PLR` | Light kit mode / random non-off |
| `:PS<0-100>` | Play stored sequence ★ replays every step |
| `:PX` | E-STOP ★ latches; no-op while latched |

### A.2 Configuration `#P…`
`#PSC` calibrate · `#PID<n>` · `#PZERO` · `#PFACTORY` ★ clears calibration too · `#PRESTART` ·
`#PDEBUG<0|1>` · `#PWIFIRESET` · `#PWIFI[0|1]` · `#PROTARYTEST[1-10]` · `#PAGGRESSION[0-2]` ·
`#PMOTOR[0-2]` ★ resets commissioning · `#PEXPERT[0|1]` · `#PSTATUS` · `#PCONFIG` ★ replayable
`#P<KEY>=<VALUE>` lines · `#PL` list sequences · `#PD<n>` delete sequence · `#PBAUD<n>` ·
`#PR` toggle rotary (reboot) · `#PNCL/#PNOL/#PNCR/#PNOR` limit polarity ·
`#PS<n>:<elements>` store sequence (elements `P R A D M W L H`, ★ `X` allowed, legacy and full
forms accepted, canonical `:P` form stored).
New ★: `#PRESUME` (release E-STOP; deliberately not a `:P` command) · `#PSTATE` (snapshot) ·
`#PCOMMISSION[BYPASS]` · `#P<KEY>=<VALUE>` generic setter for every Settings field · `#PHB<sec>` ·
`#PMESH…` fields · `#PSEQNAME<n>=<name>`.
Phase 4 ★ (implemented): `#POVERRIDE[1|0]` 60 s rescue override (on: USB console or web only;
off: any source; refused while E-STOP latched) · `#PJOG{L|R}{+|-}[pct]` a jog for one 2 s lease
(the page renews it through `/api/jog`) · `#PJOG0` release · `#PNUDGE<±ticks>` bounded lifter
step (≤ 100 ticks, continuous at the calibrated minimum) · `#PSETHOME` the head is lined up,
make this home · `#PCOMMISSION` status, `…START`, `…STOP`, `…RUN`, `…NEXT`, `…RESET`,
`…BYPASS` · `#PSAFESPINHERE` (commissioning step 6 only). None is a chain element, so a stored
sequence can carry none of them.
Removed (dropped features): `#PRC*`, `#PREMOTE*`, `#PRNAME`, `#PRSECRET`, `#PRPAIR`,
`#PRUNPAIR`, `#PRMASTERKEY*`.

### A.3 Acceptance and dispatch rules
First byte `:` or `#` → ours; `?STOP`, `&ESTOP…`, `&SABE,ESTOP…` → E-STOP; anything else
dropped whole. ID mismatch drops the line silently. Chained elements are `:P…` each. All `:P`
motion commands except `:PL`/`:PW` end random mode. Blocking commands hold the chain; `:PM`
does not. Dedup across transports 750 ms.

### A.4 Random mode tables (unchanged from v3)
Levels: Gentle lift 30–50 %, rotary min+5..40 %, actions {big lift, random angle, look around},
waits ×2.0. Medium lift 65–85 %, rotary min+10..90 %, all six, ×1.0. Aggressive lift 80..max,
rotary 70–100 %, all six, ×0.4. Actions: pop up/down; big lift; spin at top; random angle; look
around; bob and weave. Every descent below safe height homes first, enforced by policy not by
the script.

### A.5 Light kit truth table (PCF8574 pins 1/2/3, active low)
0 full cycle · 1 off · 2 Obi-Wan · 3 Yoda · 4 Sith · 5 searchlight · 6 Dagobah · 7 sparkle.
Bits A=bit2, B=bit1, C=bit0, written inverted.

---

## Appendix B — Persistent state

### B.1 v3 NVS keys (namespace `uppityspinner`) → v4
`lftminpwr`, `lftseekbotpwr`, `rotminpwr`, `lftdist`, `minheight`, `rotenccount`, `driftpct`,
`aggression`, `motortype`, `wizstate`, `expertmode`, `wifi`, `ssid`, `pass`, `ap`, `mwifi`,
`remote*` → mapped into the v4 struct by `capture_config.py` from a `#PCONFIG` dump, not read
from NVS (different namespace, clean slate).

### B.2 v4 `Settings` (schema v1, append-only)
identity: `marcId`, `cmdBaud`, `hostname` · lifter: `motorProfile`, `minPowerUp`,
`minPowerDown`, `travelTicks`, `inverted`, `limitPolarityLifter`, `expertMode`, `driftPct` ·
rotary: `ticksPerRev` (float), `ticksPerRevConfident`, `contactArcTicks`, `chatterPerPass`,
`rotaryMinPower`, `safeSpinTicks`, `rotaryDisabled`, `limitPolarityRotary` · behaviour:
`aggressiveness`, `commissionState`, `dedupMs` · wifi: `wifiEnabled`, `apMode`, `ssid`, `pass`,
`apChannel` · mesh: `meshEnabled`, `oct2`, `oct3`, `meshPass`, `wcbQty`, `deviceId`,
`meshChannel`, `checksum`, `hbPeriodS` · ui: `theme`.
Calibration blob: `slotValidUp[21]`, `velocityUp[21]`, `overshootUp[21]`, same for down,
`measuredAt`. Sequences `seq00..seq99`; names `nam00..nam99`.

### B.3 v3 EEPROM caveat
`EEPROMSettings.h` only commits when `EEPROM_FLASH_PARTITION_NAME` is defined (it never is),
and `EEPROM.begin(EEPROM.length())` runs before any `begin()`. Calibration tables, limit
polarity, ID, baud, invert and sequences may never have survived a reboot on v3. Confirm on a
boot log before promising any v3 migration beyond the NVS tunables.

---

## Appendix C — Ecosystem facts (verified 2026-09-07 from local clones)

### C.1 Sabé conventions to mirror
Arduino + `sketch.yaml`; no ReelTwo; core `WebServer.h`; gzipped PROGMEM assets built by
`build-c6-assets.py`; hand-rolled JSON with an escaper; `Preferences` blob, schema v16,
append-only with migrations; `SabeLog` tee → 16 KB ring; control task core 1 prio 10,
`loop()` core 1 prio 1; no flash writes while armed; `POST /api/estop` unauthenticated; token
ring auth on other mutations; state pill states `SAFE_IDLE/OPERATIONAL/FAILSAFE_WARN/
EMERGENCY_STOP/NO_LINK`; version `0.MINOR.PATCH-tag`; GPL-2.0 (the periscope stays LGPL-2.1).
Sabé's repo is private; cite local paths, not URLs:
`…/PROJECTS/R2-D2 Astromech Droid/Control/Amidala Claude Project/Sabe`. Roam-A-Dome ALT:
`…/PROJECTS/R2-D2 Astromech Droid/Roam-A-Dome/Roam-A-Dome_ALT` (public).

### C.2 `&` protocol (DroidNet namespace) and the periscope's messages
Existing: `&SABE,HB,<fw>,<uptimeS>,<state>,<driveAgeMs>,<domeAgeMs>` (10 s default, settable
5–300); `&SABE,STATE,<state>,<scale>`; `&SABE,ESTOP,<reason>`; `&SABE,ACK/NAK,<cmd>[,<reason>]`;
`&SABE,HELLO,<fw>,<proto>`; `&SABE,BATT,…`. `&RAD,HB,<fw>,<uptimeS>,<state>` broadcast 10 s;
`&RAD,POS,<deg>,<mode>` and `&RAD,FAULT,<code>` unicast to special peer 20.
Inbound verbs Sabé accepts: `&ESTOP`, `&ESTOP,RESET`, `&DISABLE`, `&STATUS`, `&HELLO`, `&SAY`.
Unknown verbs get no answer (NAK spam was the largest source of mesh junk).
**Periscope v4**: `&PERI,HB,<fw>,<uptimeS>,<state>,<height%>,<rotDeg>,<homed>` broadcast every
`hbPeriodS` (default 10, 5–300); `&PERI,POS,<height%>,<rotDeg>` unicast to 20 on change, ≤ 1 Hz;
`&PERI,STATE,<state>` on transitions; `&PERI,FAULT,<code>` ensured unicast, broadcast fallback;
`&PERI,ACK/NAK,<cmd>[,<reason>]` only for `&` verbs addressed to it. Inbound: `&ESTOP`,
`&ESTOP,RESET`, `&DISABLE`, `&STATUS`, `&HELLO`; everything else silent. Broadcast payloads
≤ 187 chars.

### C.3 WCB / WCBClient constraints
ESP-NOW; singleton; `begin()` keeps an existing SoftAP as `WIFI_AP_STA`, calls
`WiFi.disconnect()`, pins the channel only when there is no AP (so the AP must already be on
the mesh channel), overwrites the STA MAC to `02:oct2:oct3:00:00:<id>`; disable modem sleep;
device IDs 1..qty are WCBs, 20 is the special slot (`?SPECIAL,ON` on the WCBs); heartbeat 10 s,
offline after 5 misses; `structCommand[200]`; unicasts fragment to ~2.8 KB; WCB fw ≥ 6.2.0
required for device-20 visibility; grammar `?` local config, `;` execute/transfer, `^` chain,
`;W<x>;S<y><payload>` reaches "WCB x serial y" (the periscope today is `;W3;S4`). RAD ALT
vendors tag 1.13.0; Sabé builds against 1.9.10; pin one tag in `libs/`.

### C.4 DroidNet Signal Booster and Command Library
Booster = Pi talking USB serial to one WCB; passive tap of all forwarded traffic; no MQTT/HTTP
to boards. Victron telemetry available to boards via `?VSTATS` → one JSON line ending `\r`
(≥ 500 ms between requests). Command Library: `libraries/boards/<id>.json` with components
(`kind` device-native | wcb-verb, `confidence` high | community | low), commands (`template`,
`params`, `safety` cosmetic | movement | power | config, `category`), enums; `manifest.json` +
`releases.json` (`libraryVersion` 4.2.0 as of 2026-07-22). Current periscope file: 41 commands,
fw 3.5.3, confidence community. The copy in the v3 repo root uses the old `group` schema and
must not be used as a base.

### C.5 Sabé design tokens
paper: `--bg #F3EEE2 --raise #FBF9F3 --raise2 #ECE6D8 --well #EAE3D4 --field #FFFEFA --text
#211C17 --accent #7A2230 --accent-hi #93293A --ok #38754F --warn #8F6B22 --bad #93293A`.
slate: `--bg #15171C --raise #1C1F25 --raise2 #262A31 --well/--field #101216 --text #E9E9E6
--accent #A93246 --accent-hi #C7475B --ok #55A877 --warn #D8AC52 --bad var(--accent-hi)`.
Fonts `"Schibsted Grotesk","Hanken Grotesk","Helvetica Neue",system-ui` / `"JetBrains Mono",
"SF Mono",Menlo`. `html{font-size:20px}`; cards 12 px radius; nav 11 rem; `main` max 1100 px.

---

## Appendix D — Lifter motor profiles (data, unchanged)

| Field | Pololu 4757 6.3:1 | Pololu 4751 19:1 | IA-Parts |
|---|---|---|---|
| default min power / seek-bottom power | 65 / 40 | 45 / 30 | 30 / 30 |
| default travel ticks | 450 | 1350 | 845 |
| max throttle / calibration sweep cap | 1.0 / 100 | 0.75 / 85 (1.0 / 100 in expert) | 1.0 / 100 |
| stall timeout ms | 2000 | 700 | 2000 |
| slow-progress min ticks/s, max ms | 10, 3000 | 25, 1500 | 10, 3000 |
| pulse on/off ms | 3 / 1 | 5 / 2 | 3 / 1 |
| breakaway max ms / ramp ticks cap | 500 / 80 | 250 / 40 | 500 / 80 |
| calibration cooldown ms | 2500 | 4000 | 2500 |
| commissioning/raise speed | 0.8 | 0.6 | 0.8 |
| soft approach | off | on, 15 % | off |

Rotary has no profile; `RotaryCharacterisation {ticksPerRev, contactArcTicks, chatterPerPass,
minPower, creepStepTicks, rampAccelScale, rampDecelScale, rampLatencyMs, stopDistanceTicks,
confident}` replaces the unmodelled state. Recommended motors 12 V #4841/#4847, 6 V #4801/#4807.

---

## Appendix E — Pin map (standard board; from `pin-map.h`)

Lifter: encoder A/B 34/35, PWM1/PWM2 32/33, DIAG 36, top limit 18, bottom limit 19.
Rotary: encoder A/B 27/13, PWM1/PWM2 25/26, DIAG 39, home switch 23.
Status LED 5. Command UART RX 16 / TX 0 (GPIO0 is a strap pin; TX only if wired).
I2C SDA 21 / SCL 22; PCF8574 at 0x20, INT 17, pins numbered 200–207: 200 input A,
201/202/203 light kit A/B/C, 204 motor EN, 205 motor ENB, 206 input B, 207 input C.
The dropped SD-card variant moved the limits and enables onto the expander; the HAL keeps an
"inputs on expander" path possible but the standard board is the tuned target.

---

## Appendix F — Motion and safety core specification

### F.1 Module responsibilities (pure core)
`Hal` (F.7); `Profiles`; `Knowledge` (lifter `{ticks, UNKNOWN/KNOWN/STALE, travelTicks,
travelCalibrated}`, rotary `{ticks, HomeConfidence, HomeProvenance, ticksPerRev, lastEdgeArc,
lastEdgeClosures}`); `SafetyPolicy` (`evaluate(request, knowledge, faults, override,
commissioning) → Verdict`, `descentFloorTicks`, `rotaryAllowed`); `EstopLatch` (`trip(Source)`,
`tripped()`, `release()`, `source()`, `seq()`, holds the `panicStop` hook); `FaultModel`;
`LifterAxis` (Seek, Creep, Hold, Calibrate, bootResolve); `RotaryAxis` (MoveToTicks,
Absolute, Relative, Spin, Jog, Home, Characterise); `Composites` (SafeLower, RaiseThenRotate,
FirstUse); `MotionSupervisor` (`begin(nowMs)` runs bootResolve; `tick(nowMs)`); `CommandQueue`
(8-deep MPSC ring of `MotionRequest` with a `Lock` interface; atomics `cancel`,
`jogKeepaliveMs`, `overrideUntilMs`; E-STOP does not use the queue); `StatusSnapshot`
(seqlock publish/read); `EventLog` (SPSC ring of `Event{code,a,b,c,tMs}`, formatted in
`loop()`); `Sequencer`.

Every machine: `start(request, knowledge) → Verdict`, `step(Inputs) → Output`, `result()`.
`Inputs` = {nowMs, lifterTicks, rotaryTicks, limitTop, limitBot, homeSw (debounced), homeSwRaw,
diagL, diagR, estop, cancel, enableState}. `Output` = {lifter and rotary `Coast|Brake|Drive(±duty)`,
enableRequested}. Debounce (once per tick, shared): limit closed = 3 consecutive closed, open =
20 ms open; DIAG = 2 consecutive low; home closed = 2 consecutive, released = 30 ms open.

### F.2 Timing
FreeRTOS task, core 1, priority 10, `vTaskDelayUntil` 1 ms (`CONFIG_FREERTOS_HZ=1000`). Rejected:
`esp_timer` (core 0 affinity, competes with WiFi), GPTimer ISR (LEDC not IRAM-safe here, state
machines would need to be ISR-safe), RMT carrier tricks (untestable, speed changes mid-phase).
LEDC 10 kHz / 10-bit on 32/33/25/26, drive duty kept inside 12–88 % (0 and 100 % are steady levels); `ledc_update_duty` lands at the next carrier period.
Allowed in the tick: two PCNT reads, five GPIO reads, up to four LEDC writes on change, the
pure machines, one ~120-byte snapshot publish, event pushes. Debug builds count `Wire` calls
from the supervisor task and assert zero. Expected cycle < 50 µs, placement ±100 µs, except
during flash erase (tens of ms, both cores) which `Persist` prevents while armed. Task WDT 1 s.

### F.3 State machines
**LifterSeek(target, speed).** Start: clamp speed to `[minPower, effectiveMax]`; clamp target
to `[descentFloor, travel]`; a changed target yields `CLAMPED(reason)` and an event
`SEEK CLAMPED 0 -> 891 (66%) reason=ROTARY_NOT_HOME`; lifter UNKNOWN and down → `REFUSED`;
< 5 ticks and not a limit seek → `DONE`. States: WaitEnable → Breakaway (continuous until
10 ticks or `breakawayMaxMs`, skipped under 30 ticks) → Ramp (off-ticks grow over
`clamp(distance/3, 20, rampCap)` in the three bands 0–75/75–90/90–100 %) → Cruise (on/off from
the profile; soft approach interpolates to `seekBottomPower` inside `max(30, distance×pct)` up
when enabled and the last 20 % down always; target pre-compensated by `overshoot[slot]` when
calibrated). Exits in order: estop, cancel, limit in direction (brake 100 ms, re-reference,
`DONE(LIMIT)`), target crossed (not for full-travel seeks), DIAG, stall (`stallTimeoutMs` with
no ticks), encoder against drive > 8 ticks (`ENCODER_DIRECTION`), slow progress (one re-boost,
then `STALL(slow)` after `slowProgressMaxMs`). Done with target > 1 % hands off to Hold.
**LifterCreep(dir, power, mode).** Continuous, never pulsed. Wizard: `clamp(max(0.30×ceiling,
defaultMinPower), ceiling)`, 1500 ms stall, 30 s cap. Jog: caller's throttle (floor 0.10), no
stall exit, 2 s keepalive lease. Down below the floor needs home or override; UNKNOWN allows up
only, override or not.
**LifterCalibrate.** P1 travel: creep up/down twice; accept when each pair agrees within 5 %
(no magnitude floor); up count is the reference; >10 % up/down asymmetry logged as EMI hint.
P2 min power per direction: step down 5 % from the profile default while breakaway still
succeeds; floor + 5 %. P3 per 5 % slot from min to sweep cap: pulsed cruise to mid-travel,
record velocity and overshoot (coast after the last drive tick, 200 ms window), both
directions; invalid slots fall back to the highest valid slot below (v3 policy kept); DIAG →
`FaultWait` ≤ 15 s with an EN cycle. P4 return to bottom, persist when disarmed, recommend
`minUp + 5`. Any console byte, `:PX` or `/api/stop` aborts.
**bootResolve.** 50 ms sample; bottom → 0 KNOWN; top → travel KNOWN (profile default if
uncalibrated, flagged); neither → UNKNOWN (up-only, refusals say `LIFTER_UNKNOWN: send :PP100
to reference`); both → `LIMIT_CONTRADICTION`; rotary switch closed → zero, `SWITCH_LIVE`.
Boot log: `BOOT lifter=TOP(known 1350) rotary=SWITCH_LIVE limits=[top=1 bot=0 home=1]
diag=[ok ok] ticksPerRev=1185.2(confident)`.
**RotaryMoveToTicks(target, speed, max).** Absolute in the current frame, never normalised.
Ramp every 25 ms by |Δ|/100 accelerating, |Δ|/20 decelerating; distance scaling when
`max > speed`; floor `minPower`; brake when remaining ≤ characterised stopping distance;
arrival = within 5° for 3 ticks at rest; overshoot beyond fudge → reverse at min power once,
second reversal → `DONE(OVERSHOOT err)`; stall = velocity < 20 % of characterised for 400 ms
(or no ticks 500 ms before characterisation); policy re-checked per tick.
**RotaryAbsolute** needs `LATCHED` or `SWITCH_LIVE`; otherwise the composite prepends Home
(and Characterise(2) if `ticksPerRev` unknown); `delta = shortestDelta`.
**RotaryRelative**: `target = current + deg×ticksPerRev/360`, any confidence, speed honoured.
**RotaryHome.** Pass 0: switch closed at rest → zero (UNKNOWN) or correct the frame with
`HOME FRAME CORRECTED by N ticks (switch wins)` (LATCHED, |deg| > tol) → `DONE`. Pass 1 (only
with a reference): move to 10° before zero on the side that makes pass 2 always sweep the same
direction, at 0.5, brake 50 ms; failure is not fatal. Pass 2: adaptive pulses targeting
`creepStep = max(2, ticksPerRev/180)` ticks (4 ms start, ×1.5+1 when < step/2, ×2/3 when
> 2×step, bounded 4–60 ms, 12 ms settle, switch polled every tick); on closure record `enter`,
continue until release or 40 ticks past entry, brake, `exit` = last closed sample, zero at
`(enter+exit)/2`, record arc and closures, `LATCHED` with provenance `EDGE_CENTRE`. Sweep
> 1.25 rev (2000 ticks if unknown) with no closure → reverse once; second miss →
`FAILED(NO_EDGE swept=N)`; 12 no-move pulses → `STALL`; closed for > 60 ticks →
`FAILED(SWITCH_STUCK_CLOSED)`; 25 s backstop per sweep. On failure the frame is untouched and
confidence becomes `STALE` (was LATCHED) or stays `UNKNOWN`. Pass 3: if |deg| > 3°, one move to
0 at min power ≤ 2 s; any failure logged `SETTLE SKIPPED` and ignored.
**RotaryCharacterise(revs).** Prepends RaiseThenRotate; continuous at `minPower + 10 %`;
trailing-edge debounce; ticks/rev from successive `enter` deltas; confident at revs ≥ 3 and
spread ≤ 2 %, probable at revs ≥ 2 and ≤ 5 %, else not repeatable; reports arc mean/min/max,
chatter, creep-step-vs-arc verdict; writes `ticksPerRev`, `contactArcTicks`, `chatterPerPass`
when confident or probable (old→new logged); ends `LATCHED` from the last pass; no edge within
1.25 rev → `FAILED(NO_SWITCH)` with the polarity hint; deadline `revs×20 s + 10 s`.
**RotarySpin / RotaryJog.** Same machine: Spin floors at `minPower`, stalls stop it, ends on the
next command, `:PR0` → Home; Jog floors at 0.10, no stall exit, 2 s lease, override-gated.
**SafeLower(speed).** UNKNOWN → seek top. KNOWN at or below safe height → skip homing (the head
is inside the opening, therefore aligned). At bottom → `DONE`. Not home → Home; failure → stay
raised, `FAILED(HOME_FAILED)`, fault visible; no retry loop. Lights off (queued). Seek 0 at
`max(minPower, speed)`.

### F.4 HomeConfidence
`UNKNOWN` (no reference this power cycle; boot always resets to it). `SWITCH_LIVE` (debounced
closed now, motor off: definitive, ±3 ticks; entering it from UNKNOWN zeroes the encoder).
`LATCHED_ENCODER` (edge captured or rest zero taken this power cycle, encoder continuous
since; "at home" = |deg| ≤ 10°). `STALE` (latch exists but a stall, DIAG while rotating,
phantom-tick threshold, a > 5 % ticks-per-degree disagreement, or a failed home after pass 1
moved the head; frame kept, no descent on it). Open-at-rest is expected at true home and never
demotes. Motor disable never demotes (gearing and lead screw hold position). A rest-contact
read while LATCHED with |deg| > 10° corrects the frame loudly: the switch always wins.
`rotaryDisabled` reports LATCHED with provenance NONE.

| Motion | UNKNOWN | SWITCH_LIVE | LATCHED | STALE |
|---|---|---|---|---|
| Descend below safe height | refused → home first | allowed | allowed if |deg| ≤ 10° | refused → re-home |
| Absolute rotate | home first (+ characterise) | allowed | allowed | home first |
| Relative rotate, spin, jog | allowed | allowed | allowed | allowed |
| Calibrate lifter | refused unless rotary disabled | allowed | allowed | refused |

`rotaryAllowed` requires lifter KNOWN above safe height, or the top limit physically closed.

### F.5 E-STOP
`trip(source)`: atomic set, `panicStop()`, record source/time, bump seq; idempotent, any
context, never blocks. Supervisor: job `ABORTED(ESTOP)`, both axes brake then coast,
enable off, queue flushed, sequencer/chain/`:PW`/random/commissioning cancelled; knowledge
preserved (PCNT keeps counting; the mechanism holds). While latched: motion refused with
`ESTOP LATCHED (source=web at t=…): #PRESUME or Reset`; `:PL`, status and config reads work.
Release via `#PRESUME`, `POST /api/estop/reset`, mesh `&ESTOP,RESET` → IDLE, coasting, nothing
queued, nothing resumes. Soft stop (`POST /api/stop`) cancels the job and flushes the queue
without latching. Internal faults are not E-STOPs; they stop the affected job through the
fault model.

### F.6 Fault model
| Fault | Detect | Effect | Clears |
|---|---|---|---|
| `LIFTER_DIAG` / `ROTARY_DIAG` | pin low 2 ticks | axis brake→coast, job `FAULT(DIAG)`, EN cycle (low 50 ms) | DIAG high ≥ 100 ms after the cycle; calibration waits ≤ 15 s |
| `DIAG_REPEATED` | ≥ 3 in 60 s | latching, motion refused | `clearfault` |
| `STALL` | per machine | job fails, axis → STALE | next limit contact / successful home |
| `SLOW_PROGRESS` | velocity below profile | one re-boost, then STALL | — |
| `ENCODER_DIRECTION` | ticks against drive > 8 | job fails, lifter STALE | manual (invert/wiring hint) |
| `ENCODER_NOISE` | > 5 idle ticks in 10 s | counter; > 20/min → STALE, drift correction off | re-reference |
| `LIMIT_CONTRADICTION` | both closed ≥ 3 ticks | latching while present, lifter refused | when it stops, then re-reference |
| `LIMIT_MISMATCH` | limit closes > 25 % of travel away | trust the switch, re-reference, travel suspect | — |
| `HOME_SWITCH_STUCK` | closed > 60 ticks of travel | home fails | next open read |
| `EXPANDER_FAULT` | I2C NACK/timeout or no ack in 50 ms | job fails, cannot arm | next successful transaction |
| `TICK_OVERRUN` | cycle > 800 µs or missed tick | counters in the snapshot | — |
| `WDT` | 1 s | reset (drivers disabled by expander power-on state) | boot |
| `PERSIST_WHILE_ARMED` | request while armed | deferred, logged | on disarm |

### F.7 HAL sketch
```cpp
namespace uppity {
enum class Axis : uint8_t { Lifter, Rotary };
enum class Bridge : uint8_t { Coast, Brake, Drive };
struct BridgeCommand { Bridge mode; int16_t duty; };   // duty -1000..+1000 (0.1 %)
struct RawInputs { int32_t lifterTicks, rotaryTicks; bool topLimit, bottomLimit, homeSwitch;
                   bool lifterDiag, rotaryDiag; bool enableAcked; };
class Hal { public:
  virtual uint32_t nowMs() const = 0;  virtual uint32_t nowUs() const = 0;
  virtual void readInputs(RawInputs&) = 0;                 // tick-safe
  virtual void writeBridge(Axis, BridgeCommand) = 0;        // tick-safe, on change
  virtual void panicStop() = 0;                             // any context
  virtual void requestEnable(bool) = 0;                     // atomic; I2cService acts
  virtual void setEncoder(Axis, int32_t) = 0;               // PCNT clear + software offset
  virtual void setLifterInverted(bool) = 0;                 // swaps PWM pins and count sense
};
struct Lock { virtual void lock() = 0; virtual void unlock() = 0; };
}
```
`Esp32Hal`: one PCNT unit per axis, channel edge = A positive edge (hold on negative), level =
B (keep high, invert low), `max_glitch_ns = 10000`, ±32767 with `accum_count`; units created
from the supervisor task so the overflow ISR lands on core 1; never link `ESP32Encoder`.

### F.8 Simulator
`LifterModel {breakawayDuty, breakawayDwellMs, kv, runningFriction, coastDecel, brakeDecel,
backdriveGravity/backdriveFriction (a moving mast that falls on with the outputs floating; 0 =
self-locking), travelTicks, limit hysteresis/bounce, starvationWindowMs, phantomRate (driving/idle), DIAG
injection incl. thermal trip, encoderFrozen, inverted, boot state}`. `RotaryModel {ticksPerRev
(float), stick-slip: pulses < minPulseMs produce nothing else a burst in [5, 20] ticks, cam
switch: centre, halfWidthMoving 8, halfWidthRest 2 below restVelocity, chatterProbability ≈ 7
make/break per pass, direction-dependent first contact from geometry, stuckClosed,
disconnected, polarityWrong, phantoms, DIAG}`. `SimHal` applies polarity, models the expander
(`enableLatencyUs`, `failEnable`), counts I2C-equivalent calls per context, records every
bridge command with its timestamp. `Rig` wires SimHal + supervisor + queue + sequencer with a
fake clock and `runMs(n, cb)`.

### F.9 Keep from v3, and one datasheet check
Keep near-verbatim: `advanceEstopFsm` and its RX-callback placement; `SerialFifo`; the
per-line acceptance rule; the three profile tables with their rationale comments; the
self-test's trailing-edge debounce and report text; the breakaway three-band ramp; the
two-flip overshoot rule; the highest-valid-slot fallback; the `rotaryAtHome()` comment block as
README text; the web log buffer idea; the LED colour table. The TB9051FTG datasheet
(read 2026-09-07) confirms PWM1=PWM2=low is a short brake and High-Z needs EN low, so
`Bridge::Brake` is the PWM-level stop and `Bridge::Coast` is an enable-off request.
