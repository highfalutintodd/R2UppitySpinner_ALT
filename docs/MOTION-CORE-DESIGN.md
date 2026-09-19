# Motion-control and safety core design (v4)

Generated 2026-09-07 as the design pass behind Appendix F of `docs/REWRITE-PLAN.md`. Where this document and the plan disagree, the plan is authoritative and this file is corrected; but this file carries the detail the plan condenses.


# Uppity Spinner rewrite — motion-control and safety core design

Reference firmware: `"/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/R2UppitySpinnerV3.ino"` (v3.6.0), handoff at `docs/REWRITE-HANDOFF.md` in the same folder.

## 0. What the code review found that the design must answer

These are verified in the source, not inferred from the handoff, and each maps to a structural decision below.

1. **Every motion primitive blocks on core 1 and is also called from core 0.** `seekToPosition` (1654), `rotateHome` (2299), `rotateUntilHome` (2381), `calibrate` (3471), `rotaryHomeSelfTest` (2662), `wizardCreepToLimit` (3404), `safetyManeuver` (3186) are `for(;;)`/`while` loops with `delay()`. The web handlers in `WebPages.h` call them directly from `eventLoopTask` on core 0: `/api/action?do=homerotary` → `rotateHome()` (2284), `/api/wizard/creep` → `wizardCreepToLimit()` (2603), `/api/wizard/calibrate` → `calibrate()` (2618), `/api/wizard/seek` → `seekToPosition()` (2661). The web server is single-threaded inside `webServer.handle()`, so while a wizard handler is inside a blocking primitive, `/api/estop` cannot be serviced. That is the ESTOP-unreachable-during-wizard bug.
2. **The rescue-override race is a two-writer problem.** `/api/rotary` on core 0 calls `rotaryMotorSpeedForce()` (2200); `loop()` on core 1 calls `animate()` → `rotaryMotorUpdate()` (2016), whose first check is `if (!rotaryAllowed() && fRotaryThrottle != 0) rotaryMotorStop()` — and `rotaryAllowed()` (1998) knows nothing about `sRescueOverrideExpiry`. The jog is cancelled within 25 ms by a policy evaluated in a different context from the one that issued the drive.
3. **Four definitions of "home" at different sites:** raw `rotaryHomeLimit()` (1309, used by `/api/lift`'s descent clamp at 2221 and status), `rotaryHomeLimitConfirm()` 30 ms window (1326), `rotaryAtHome()` switch-or-latched-encoder-within-10° (1354, used by `seekToPosition`'s clamp), and `isRotaryAtRest()` switch-or-deg==0-and-not-moving (2882, used by `seekToBottom`, `seekToTop(PID)`, `calibrate`, `safetyManeuver`).
4. **A failed home still re-zeroes the encoder.** `rotateHome()` at 2357: `if (!rotaryAtHome() && count >= 100) { "HOME FAILED ..."; resetRotaryPosition(); }`. It no longer claims success, but it still moves the encoder frame on failure, which the handoff forbids.
5. **The calibration tables are dead weight.** `seekToPosition` sets `limit = 0` in both directions (1708, 1851) and never consults `fUpLimits`/`fDownLimits` beyond `getUpOutputLimit()` returning "some slot is valid". The sweep spends minutes of motor heat producing `outputLimit` numbers nothing reads. What the seeks actually need is min power per direction, a repeatable travel count, and stopping distance per speed — none of which the sweep records.
6. **The motor-enable pins are hammered over I2C inside the pulse loop.** `lifterMotorStop()` (1497) and `rotaryMotorStop()` (2852) call `enableMotors()` unconditionally, and `enableMotors()` does two `PCF8574::digitalWrite` transactions (1425–1426), each a full `beginTransmission/write/endTransmission` (`PCF8574.cpp:1013`). At 3 ms on / 1 ms off that is two blocking ~250 µs I2C transactions every 4 ms, inside the timing-critical pulse pattern, sharing `Wire` with the web task's expander reads.
7. **PWM carrier is 1 kHz.** `core/AnalogWrite.h` is commented out (445), so `::analogWrite` is the core's: `esp32-hal-ledc.c:771` `analog_frequency = 1000`, 8-bit. A 3 ms "on" pulse is three carrier periods; a 1 ms "off" is one. Pulse energy is quantised to ±33% depending on carrier phase. The TB9051FTG accepts PWM up to 20 kHz.
8. **Encoders count one edge of A in a GPIO ISR** (4649–4692), the arrangement most exposed to EMI; phantom ticks after reset are documented at 2350. The installed core (esp32 3.3.4, IDF 5.5 libs) ships `driver/pulse_cnt.h` for the classic ESP32 with an 8-unit PCNT that has a hardware glitch filter.
9. **ESTOP is abort-and-continue and consumable.** `sWebAbort` is set by the RX scanner (1110) and `/api/estop` (2249), consumed and cleared by whichever `serialAbort()` sees it first (1636–1641), cleared again by `/api/estop` after a 50 ms `delay` (2258–2259) and by the `:PX` dispatcher (4888). Queued chain steps run after it.
10. **`:PD` is `absolute(current + rel)` with `normalize()`** (2292, 2278), so ≥180° is impossible and the speed arguments are dropped at 5061.
11. **NVS writes happen while motors are enabled**: `sLifterParameters.save()` mid-`safetyManeuver` (3314), `wizardAdvance()` and `/api/params/save` at any time, `sSettings.write()` at end of `calibrate()`. `CONFIG_SPI_FLASH_AUTO_SUSPEND` is absent from the pinned core's sdkconfig (it is not supported on classic ESP32 at all), so every commit halts both cores' non-IRAM code while LEDC keeps emitting the last duty.
12. **The boot problem** is exactly `rotaryAllowed()` gating on `getLifterPositionClamped() > fRotaryMinHeight` (2003) with an encoder that reads 0 at boot; the limit switches are read (`lifterTopLimit()`) but never used to establish position.

## 1. Module decomposition

Two layers with a hard line between them: a **pure core** (`namespace uppity`, C++17, no Arduino headers, compiled on the Mac by `test/native/Makefile`) and **Arduino glue** that owns every ESP-IDF call. The pure core never sees `millis()`, `Serial`, `Wire`, `Preferences`, or FreeRTOS.

### 1.1 Pure core (`src/`)

| Module | Responsibility | Notes |
|---|---|---|
| `Hal.h` | Abstract interface the core drives: encoders, PWM, limit/DIAG inputs, enable request, clock. | Sketch in §7.1. Two implementations: `Esp32Hal` (glue) and `SimHal` (tests). |
| `Profiles.h` | `LifterMotorProfile` (kept from today's table, same fields and values), new `RotaryCharacterisation` {ticksPerRev (float), contactArcTicks, chatterPerPass, minPower, creepStepTicks, rampAccelScale, rampDecelScale, rampLatencyMs, stopDistanceTicksAtMinPower, confident:bool}. | The rotary finally gets a model; it is *measured*, never assumed. |
| `Knowledge.h` | `LifterKnowledge` {ticks, confidence UNKNOWN/KNOWN/STALE, travelTicks, travelCalibrated, referencedAt}, `RotaryKnowledge` {ticks, `HomeConfidence`, `HomeProvenance`, ticksPerRev, lastEdgeArc, lastEdgeClosures}. | §4. |
| `SafetyPolicy.h` | Pure functions: `evaluate(request, knowledge, faults, override, commissioning) -> Verdict`, `descentFloorTicks(...)`, `rotaryAllowed(...)`. All five invariants plus wizard gating and the rescue override live here and nowhere else. | Every job asks the policy on start and every tick (position changes mid-job). |
| `EstopLatch.h` | One-way latch: `trip(Source)`, `tripped()`, `release(ReleaseToken)`, `source()`, `seq()`. Owns the "PWM zero from any context" hook (a HAL `panicStop` function pointer set by glue). | §5. |
| `FaultModel.h` | Fault enum, debouncers (DIAG, limit contradiction), counters, self-clear rules, latching set. | §6. |
| `LifterAxis.h` | State machines: `LifterSeek`, `LifterCreep` (jog and wizard creep are the same machine with different gating), `LifterHold`, `LifterCalibrate`, plus `bootResolve(limits) -> LifterKnowledge`. | §3.1–3.4. |
| `RotaryAxis.h` | `RotaryMoveToTicks` (shared engine), `RotaryAbsolute`, `RotaryRelative`, `RotarySpin` (`:PR`), `RotaryJog` (rescue), `RotaryHome`, `RotaryCharacterise`. | §3.5–3.9. |
| `Composites.h` | `SafeLower` (`:PH`), `RaiseThenRotate` (wraps any rotary job with "raise to top if lifter unknown/low"), `FirstUse` (characterise if `ticksPerRev` unknown). | §3.10. |
| `MotionSupervisor.h` | The single motor authority. Owns both axes, knowledge, policy, latch, faults, the one active `Job` slot, the `CommandQueue` consumer, the `StatusSnapshot` publisher and the `EventLog` producer. One entry point: `tick(nowMs)`. | Runs only in the supervisor task. |
| `CommandQueue.h` | Fixed-size MPSC ring of `MotionRequest` (8 deep) with a `Lock` interface (portMUX in glue, no-op on host). Plus out-of-band atomics: `cancel`, `jogKeepaliveMs`, `overrideUntilMs`. ESTOP does **not** go through the queue. | Producers: serial dispatcher, web, mesh, sequencer. |
| `StatusSnapshot.h` | POD struct + seqlock publisher/reader (`publish(const Snapshot&)`, `bool read(Snapshot&)`). | Readers never block the tick. |
| `EventLog.h` | SPSC ring (64) of `Event{code:uint8, a,b,c:int32, tMs}` + a host-testable formatter to text. No formatting in the tick. | Non-aliasing by construction: one record per transition/edge. |
| `Sequencer.h` | Chains (`:P…:P…`), stored sequences, `:PW` waits, random mode (aggressiveness levels), the wizard script. Consumes snapshots/job results, produces `MotionRequest`s one at a time. | Runs in `loop()`. Lifted from Roam-A-Dome's `Sequencer` shape. |
| `CircularMath.h` | `normalizeDeg`, `shortestDelta`, `withinArc`. | Reuse Roam-A-Dome's `src/CircularMath.h` verbatim. |
| `sim/LifterModel.h`, `sim/RotaryModel.h`, `sim/SimHal.h` | Mechanism simulator. | §7.2. |

### 1.2 Arduino glue (`glue/`, plus the `.ino`)

| Module | Context | Responsibility |
|---|---|---|
| `Esp32Hal.cpp` | supervisor task (reads/PWM), `loop()` (I2C) | PCNT units for both encoders (glitch filter 10 µs, ×1 decode so today's tick scales survive), LEDC at 20 kHz / 10-bit on 32/33/25/26, direct GPIO for 18/19/23/36/39, `esp_timer_get_time()` clock. Exposes `panicStop()` = four `ledcWrite(pin, 0)` for the latch. |
| `SupervisorTask.cpp` | own FreeRTOS task, core 1, priority 10, stack 8192 | `vTaskDelayUntil` 1 ms loop calling `MotionSupervisor::tick`. Subscribes to the task WDT and pets it every tick. Measures cycle µs into the snapshot. |
| `I2cService.cpp` | `loop()` core 1 | Sole owner of `Wire`/PCF8574: applies `enableRequested` → EN/ENB and acks via atomic `enableState`; light-kit A/B/C; expander inputs on INT (`sDigitalReadAll` pattern kept); on the SD-card variant also publishes debounced limit bits into an atomic byte the tick reads. Has a timeout and reports `EXPANDER_FAULT`. |
| `SerialFront.cpp` | UART event task (RX callbacks) + `loop()` | `advanceEstopFsm` kept verbatim, action becomes `EstopLatch::trip(SerialUsb/SerialCmd)`. Per-source `SerialFifo` (kept), per-source line assembler with the ':'/'#' per-line acceptance rule (kept), per-source dispatcher state (fixes the shared `sBuffer/sPos`). Dispatcher hands parsed commands to `Sequencer`. |
| `WebFront` (`WebPages.h` rewritten handlers) | core 0 `eventLoopTask` | Handlers **only** enqueue `MotionRequest`s or read the snapshot. `/api/estop` → `EstopLatch::trip(Web)`. `/api/stop` (new) → `CommandQueue::cancel`. Jog endpoints stamp `jogKeepaliveMs`. Nothing in this layer touches a motor. |
| `MeshFront` | WiFi task core 0 (ESP-NOW/SMQ/TCP callbacks) | `?STOP`/`&ESTOP` → `EstopLatch::trip(Mesh)`; other commands → `Sequencer` via a thread-safe line queue. |
| `Persist.cpp` | `loop()` | NVS/EEPROM writes only from a `PersistQueue` drained when the snapshot says `disarmed` for ≥100 ms; sets `persistBusy` (atomic) around the commit; the supervisor refuses to arm while `persistBusy`. OTA start refused while armed. |
| `LogSink.cpp` | `loop()` | Drains `EventLog` to Serial and `WebLogBuffer` (kept). |
| `StatusLed.cpp` | `loop()` | Existing colour table, driven from the snapshot. |

### 1.3 Who talks to whom (context map)

```
UART RX cb (UART event task) ──:PX──▶ EstopLatch ◀──/api/estop── Web (core 0)
        │ bytes                            ▲                        │ enqueue / read snapshot
        ▼                                  │ ?STOP/&ESTOP           ▼
  SerialFront ──▶ Sequencer ──▶ CommandQueue ──▶ MotionSupervisor::tick (core 1, prio 10, 1 kHz)
  (loop, core 1)   (loop)          (MPSC)              │  owns: LifterAxis, RotaryAxis, SafetyPolicy,
                                                       │        Knowledge, FaultModel, Job slot
                      ▲ snapshot/events                ▼
  I2cService (loop) ◀─ enableRequested ─── Hal (PCNT, LEDC, GPIO) ── StatusSnapshot (seqlock) ── EventLog (SPSC)
        │ EN/ENB, lightkit, expander inputs                              ▲                          ▲
        ▼                                                  Web/serial/LED readers            LogSink (loop)
  PCF8574 over Wire (only here)
  Persist (loop) ── NVS only when snapshot.disarmed && !job
```

Rules: **only `MotionSupervisor::tick` writes a non-zero PWM. Any context may write zero (the latch).** Only `I2cService` touches `Wire`. Only `Persist` touches NVS. Only `LogSink` touches Serial for motion events.

## 2. Timing design

**Decision: a FreeRTOS task on core 1, priority 10, `vTaskDelayUntil` with a 1-tick (1 ms) period.** The pinned core's sdkconfig has `CONFIG_FREERTOS_HZ=1000`, `CONFIG_ARDUINO_RUNNING_CORE=1`, WiFi task pinned to core 0, `esp_timer` ISR affinity core 0. This is the same idiom Sabé runs on the same core version (`SabeControl.cpp:191–208`, priority 10, `vTaskDelayUntil`).

Rejected alternatives:
- `esp_timer` at 1 kHz: callbacks run in the esp_timer task, ISR-affine to core 0, competing with WiFi; and the callback context forbids nothing useful.
- GPTimer ISR: ISR context; `ledc_set_duty`/`ledc_update_duty` are not IRAM-safe in this build (`CONFIG_LEDC_CTRL_FUNC_IN_IRAM` unset) and the state machines would have to be ISR-safe. No.
- Hardware pulse generation (RMT carrier modulation or LEDC-on-LEDC gating): the 3 ms/1 ms envelope is a 250 Hz PWM at 75% duty over a 20 kHz carrier, which RMT can in principle emit in loop mode with carrier modulation. Rejected: changing speed means reconfiguring the carrier mid-stream, the pattern must change phase by phase (ramp, soft approach), and none of it is testable on the host. The software tick is adequate and deterministic.

**Pulse mapping.** Tick = 1 ms, so `pulseOnTicks = fPulseOnMs`, `pulseOffTicks = fPulseOffMs` exactly (3/1 and 5/2 map with no rounding). Each drive phase keeps a phase counter; the output for the tick is `Drive(duty)` during on-ticks and `Brake` (or `Coast`, see §8) during off-ticks. Breakaway "continuous" = on-ticks only. Ramp = off-ticks grow 0 → `pulseOff` over `rampTicks` of travel in the three bands the current `breakawayBoost` uses (0–75%: none, 75–90%: one brake tick, 90–100%: full).

**PWM carrier: 20 kHz, 10-bit** (`ledcAttach(pin, 20000, 10)`; 80 MHz/20 kHz = 4000 counts, so 10 bits is comfortably inside). Reasons: pulse-energy quantisation vanishes (60 carrier cycles per 3 ms pulse), `ledc_update_duty` takes effect at the next carrier period so a stop lands within 50 µs instead of up to 1 ms, and it is inaudible. Consequence: calibrated minimum powers will shift; the upgrade path forces a re-run of min-power discovery (§3.3), which is a one-minute operation.

**Work allowed inside the tick** (all sub-10 µs): `pcnt_unit_get_count` ×2 (register read under the driver's spinlock), `gpio_get_level` ×5, ≤4 `ledcWrite` (only when the value changes), the pure state machines, one snapshot publish (struct copy under seqlock, ~120 bytes), event pushes. **Forbidden**: I2C, Serial, NVS, heap, `String`, WiFi, floating-point formatting, anything that can block. The `Esp32Hal` asserts this in debug builds by counting `Wire` calls made from the supervisor task handle.

**Motor enable without blocking the tick.** EN/ENB live on the PCF8574, so enabling costs a ~250 µs I2C transaction plus bus arbitration. The supervisor never does it. It sets `enableRequested` (atomic) and, if a job needs drive while `enableState != Enabled`, sits in a `WaitEnable` sub-state (bounded, 50 ms; timeout → `EXPANDER_FAULT`, job fails). `I2cService` in `loop()` observes the request, does the write, and stores `enableState`. Disable is requested after the idle timeout (2 s at rest, as today's `MOTOR_TIMEOUT`) or on ESTOP. The `Enabled` shadow is cached; the I2C write happens only on change, so the pulse loop performs zero I2C.

**ESTOP bypasses the tick.** `EstopLatch::trip()` runs in the caller's context: atomic store (release), `hal.panicStop()` (four `ledcWrite(pin, 0)` — legal from any task on either core; the LEDC driver takes its own spinlock), then record source/time. The supervisor reads the flag at the top of every tick and again immediately before each PWM write, so the widest window in which a drive write can follow a trip is the few microseconds between that check and the write, plus one carrier period. Worst case end-to-end: under 1.1 ms. The EN disable follows via `I2cService` as a second layer, not the primary stop.

**Jitter expectations.** Nothing on core 0 (WiFi, web, mesh) can preempt a core-1 task; shared-resource contention is limited to the LEDC/PCNT spinlocks (microseconds). On core 1 the only higher-priority tasks are the UART event task (priority 24, unpinned, runs for tens of µs per burst) and system tasks; `loopTask` at priority 1 cannot delay the tick. Expect typical cycle time < 50 µs and tick placement within ±100 µs, except during a flash erase/write, which stalls both cores for up to tens of ms (a 4 KB sector erase) while LEDC keeps emitting the last duty. That is why §1.2's `Persist` rule exists and is enforced, not advisory. The snapshot carries `cycleUsMax` and `lateTicks` so the claim is measurable on hardware.

**Watchdog.** The supervisor task subscribes to the task WDT with a 1 s timeout and pets it each tick. On a WDT reset the ESP32 GPIOs revert to inputs and the PCF8574 powers up with all pins high, which puts ENB high = drivers disabled, so a reset is fail-safe for the motors. Optional hardening (not required for v1): a 10 Hz IRAM GPTimer ISR that zeros the LEDC duty through the LL register API if the tick heartbeat counter stops advancing for 100 ms.

## 3. State machine specifications

Conventions: every machine has `start(request, knowledge) -> Verdict`, `step(Inputs) -> Output`, `result()`. `Inputs` = {nowMs, lifterTicks, rotaryTicks, limitTop, limitBot, homeSw (debounced closed), homeSwRaw, diagL, diagR, estop, cancel, enableState}. `Output` = {lifter: Coast|Brake|Drive(±duty), rotary: same, enableRequested}. Timeouts are expressed as ticks or ticks-of-travel; "distance-or-time" is stated per exit. Every exit emits one `Event` with the reason and the numbers named below.

Debounce inputs (computed once per tick by the supervisor, shared by all machines): limit closed = 3 consecutive closed samples (matches today's 3 ms confirm), limit open = 20 ms continuous open; DIAG fault = 2 consecutive low; home switch: closed = 2 consecutive closed, released = 30 ms continuous open (trailing-edge debounce lifted from `rotaryHomeSelfTest`).

### 3.1 LifterSeek(targetTicks, speed)

Policy on start: clamp `speed` to `[fMinimumPower, effectiveMaxThrottle]`; `targetTicks` clamped to `[descentFloor, travel]` where `descentFloor` = safe-spin height unless the rotary is home (§4). **If the clamp changes the target, the job's result is `CLAMPED(reason)` and the event says `SEEK CLAMPED 0 -> 891 (66%) reason=ROTARY_NOT_HOME`** — the "parks at 66%" symptom becomes a loud, attributed outcome instead of a silent success. Lifter UNKNOWN and direction down → `REFUSED(LIFTER_UNKNOWN_DOWN)`. Distance < 5 ticks and not a limit seek → `DONE(already there)`.

States:
1. `WaitEnable` → 2 when `enableState == Enabled` (≤ 50 ms else `FAULT(EXPANDER)`).
2. `Breakaway`: continuous `Drive(speed)` until |Δticks| ≥ 10 or limit-in-direction; timeout `fBreakawayMaxMs` ticks → `STALL(breakaway)`. Skipped when distance < 30.
3. `Ramp`: pulsed with growing off-period over `rampTicks = clamp(distance/3, 20, fBreakawayRampTicksCap)` of travel.
4. `Cruise`: on/off pulsing at `power = speed`; soft approach: within `rampZone = max(30, distance × pct/100)` of the target (`fSoftApproachPct` going up on profiles that enable it; fixed 20% going down, as today) `power` interpolates linearly to `approachPower = fLifterMinSeekBotPower`. Stopping-distance offset: the target is pre-compensated by `overshootTicks[slot]` from calibration (§3.3) when available.
5. Exits (checked every tick, in this order): `ESTOP` (output Brake, job `ABORTED`), `cancel` (same), limit in direction of travel → `Brake` 100 ms → re-reference (bottom: ticks := 0; top: ticks := travel) → `DONE(LIMIT)`, target crossed (not for full-travel seeks, which rely on the limit as today) → `Brake` → `DONE(TARGET)`, DIAG fault on lifter → `Brake` → `FAULT(DIAG)`, no encoder change for `fStallTimeoutMs` ticks → `STALL`, encoder moving against drive direction by > 8 ticks → `FAULT(ENCODER_DIRECTION)`, slow progress: velocity over the last 1000 ticks < `fSlowProgressMinTicksPerSec` while > 20 ticks from target → first time `Reboost` (one Breakaway burst, then back to Cruise), accumulated ≥ `fSlowProgressMaxMs` → `STALL(slow)`.
6. After `DONE` with target > 0: hand off to `LifterHold`; with target ≤ 1%: `Coast`, idle-disable timer starts.

Knowledge effects: a limit exit re-references and sets `KNOWN`; a stall sets `STALE` (position may be right, but it is unproven until a limit is touched).

Logs: `SEEK UP 148->1350 speed=60`, then one event per phase change, then `SEEK UP DONE pos=1350 reason=LIMIT t=2340ms`. No periodic progress prints in the tick; the snapshot carries position for pollers.

### 3.2 LifterCreep(direction, power, mode = Wizard|Jog)

Continuous drive, never pulsed. `power = clamp(max(0.30 × ceiling, fDefaultMinPower), ceiling)` for wizard creep; the web jog passes its own throttle (floor 0.10). Exits: limit in direction → `Brake` → `DONE(LIMIT)` (re-references, promotes UNKNOWN → KNOWN); ESTOP/cancel; DIAG; no ticks for 1500 ms → `STALL` (wizard only — a 10% rescue jog may legitimately stick-slip, so `Jog` mode has no stall exit, only its deadline); wizard hard cap 30 s; jog deadline `jogKeepaliveMs` (2 s after the last keepalive; the page keeps stamping while the button is held). Policy: down-jog below the safe floor requires rotary home **or** the rescue override; jog with lifter UNKNOWN is allowed only upward unless override. Override is a supervisor-owned timestamp set by `/api/action?do=override_on` / a serial equivalent, 60 s, logged on set and expiry, and nothing else in the system can cancel a job it authorised.

### 3.3 LifterCalibrate (composite; replaces the output-limit sweep)

Preconditions: commissioning allows, rotary confirmed home (§4) or rotary disabled, lifter referenced at bottom (else the composite prepends a bottom seek via Creep). Any console byte, `:PX`, or `/api/stop` aborts (keeps today's "any key aborts calibration").

Phases, each a sub-job with its own exits:
- **P1 Travel (repeatability, not magnitude).** Creep up to top → `A1`; creep down to bottom → `D1`; repeat → `A2`, `D2`. Accept if `|A1−A2| ≤ 5%·mean(A)` and `|D1−D2| ≤ 5%·mean(D)`; `travel := mean(A)` (up-count is the reference, as the existing code argues). Up/down disagreement > 10% is logged as `ENCODER ASYMMETRY (EMI?)` but does not fail. Non-repeatable → `FAILED(TRAVEL_NOT_REPEATABLE a1=.. a2=..)`, stored value kept. No tick floor: a 250-tick build passes.
- **P2 Minimum power, each direction.** From bottom, try Breakaway at `fDefaultMinPower`, stepping down 5% while it still breaks away (≥ 10 ticks within `fBreakawayMaxMs`), then the lowest passing slot + 5% margin becomes `minUpPower`; same from the top for `minDownPower`. Each attempt is followed by a return to the reference limit. Cooldown `fCalibrationCooldownMs` between attempts.
- **P3 Per-slot characterisation (5% slots from minUp to sweep cap).** Seek bottom → 50% travel with pulsed cruise at the slot; record velocity (ticks/s) and `overshootTicks` (travel after the last drive tick until the encoder settles, 200 ms window). Same downward from the top. Slots that fail to move are marked invalid; the lookup falls back to the highest valid slot ≤ requested (today's `getUpOutputLimit` policy, kept). DIAG during the sweep → `FaultWait` (§6) up to 15 s with an EN cycle via `I2cService`, then resume or `FAILED(DIAG)`.
- **P4** Return to bottom, `CalibrationResult` → `PersistQueue` (written only once disarmed). Recommend `minUp + 5` as the operating floor, as today.

Logs one line per slot: `CAL UP slot=45 v=310t/s overshoot=7t` and a summary. Result also feeds `slowProgressMinTicksPerSec` sanity (warn if the profile threshold exceeds the measured min-slot velocity, which is the false-stall case).

Compatibility note: if the team prefers a smaller first step, keep the `OutputLimit` tables but only their `valid` bit — that is all v3.6.0 uses. I recommend replacing them; the sweep then produces numbers the seek consumes.

### 3.4 BootPositionResolve (runs once, first 50 ms of the supervisor)

Sample limits for 50 ticks. Then:
- bottom closed, top open → `lifter = KNOWN, ticks := 0`.
- top closed, bottom open → `lifter = KNOWN, ticks := travel` (if travel uncalibrated, use the profile default and flag `travelCalibrated=false`).
- neither → `UNKNOWN`. Policy permits only upward motion (Seek to top, Creep up, up-jog) until a limit closes; every refused request says `LIFTER_UNKNOWN: send :PP100 (raise) to reference`.
- both closed → `FAULT(LIMIT_CONTRADICTION)`, all motion refused until it clears (polarity or wiring).
- Rotary: home switch closed at rest → `SWITCH_LIVE` with provenance `REST_CONTACT`, encoder zeroed there (the at-rest conducting zone is 2–7 ticks, so this is a ±3-tick reference, as good as a sweep). Open → `UNKNOWN`. `ticksPerRev` restored from NVS with its `confident` flag; if absent, `FirstUse` will characterise before any absolute move.
- Motors disabled; nothing moves at boot. The safety maneuver as a mandatory first motion is retired: `SafeLower` and `RaiseThenRotate` cover every case on demand, and `BootPositionResolve` covers the mast-up boot without motion.

Boot log: `BOOT lifter=TOP(known 1350) rotary=SWITCH_LIVE(rest-zero) limits=[top=1 bot=0 home=1] diag=[ok ok] ticksPerRev=1185.2(confident)`.

### 3.5 RotaryMoveToTicks (engine shared by Absolute, Relative, and Home pass 1)

Inputs: `targetTicks` (absolute in the current encoder frame, **never normalised**), `speed`, `maxSpeed`. Ramp: every `rampLatencyMs` (25) ticks, step toward the commanded speed by `|Δ|/accelScale` (100) when accelerating and `|Δ|/decelScale` (20) when decelerating or crossing zero — the existing scheme, moved into the rotary characterisation struct. Speed scaling by distance when `maxSpeed > speed` kept (`speed += (max−speed)·|dist|/180°`). Floor: `RotaryCharacterisation.minPower` (default 40%). Decel zone: begin ramping down at `stopDistance[speed]` + fudge from the target; issue `Brake` when the remaining distance ≤ the characterised stopping distance at the current speed (defaults conservative until measured). Arrival: within `fudge` (5° → ticks) for 3 consecutive ticks at rest (velocity 0). Overshoot: if the position passes the target by > fudge, reverse at min power once; a second reversal → stop and report `DONE(OVERSHOOT err=deg)` (today's two-flip hysteresis, now a documented rule). Exits: ESTOP/cancel, DIAG, stall (velocity < 20% of the characterised velocity at this power for 400 ms, or no ticks for 500 ms before characterisation) → `STALL`, policy change (lifter no longer above safe height — cannot normally happen since jobs are serial, but the check is per tick) → `ABORTED(POLICY)`. Logs start/end with target/actual/err/reason.

### 3.6 RotaryAbsolute(deg, speed, maxSpeed)

Requires `LATCHED_ENCODER` (or `SWITCH_LIVE`, which implies a zero). `STALE` or `UNKNOWN` → the composite prepends `RotaryHome`; if `ticksPerRev` is unknown it prepends `RotaryCharacterise(2)`. Target: `delta = shortestDelta(currentDeg, deg)`; `targetTicks = currentTicks + delta × ticksPerRev/360`. Shortest path preserved for `:PA`, `rotateHome` pass 1, and random mode.

### 3.7 RotaryRelative(deg, speed, maxSpeed)

`targetTicks = currentTicks + deg × ticksPerRev/360` in float, no normalisation, multi-revolution by construction; sign convention unchanged (+ = CCW). Allowed at any confidence (it is relative); confidence unchanged unless a fault occurs. Speed arguments honoured (bug 6.1.2). `:PD0` is a no-op. After a multi-rev move, `currentDeg` is `ticks mod ticksPerRev` scaled — still valid because the frame never wrapped. Stall detection is the engine's.

### 3.8 RotaryHome

Pass 0 (no motion): if the debounced switch is closed at rest → if `UNKNOWN`, zero here (`REST_CONTACT`), `SWITCH_LIVE`; if `LATCHED` and `|deg| > tolerance` → correct the frame to the switch and emit `HOME FRAME CORRECTED by N ticks (switch wins)`; `DONE`.
Pass 1 (only when a reference exists, `LATCHED` or `STALE`): `RotaryMoveToTicks` to `−approachOffset` (10° before zero, on the side chosen so pass 2 always sweeps in the **same direction**, e.g. CCW) at 0.5; `Brake` 50 ms. Failure here is not fatal; fall through.
Pass 2 (sweep, closed-loop creep): adaptive pulse targeting `creepStep = max(2, ticksPerRev/180)` ticks per pulse: pulse width starts 4 ms, ×1.5+1 when a pulse moved < step/2, ×2/3 when > 2×step, bounded [4, 60] ms, 12 ms settle between pulses, the switch polled every tick during drive and settle. Edge capture: on debounced closure record `enterTicks`; keep creeping until release (30 ms open) or until 40 ticks past entry (a stuck-closed guard) → `Brake` 50 ms → `exitTicks` := last closed sample. `zero := (enter + exit)/2` → encoder frame shifted so that point is 0. `lastEdgeArc = |exit − enter|`, `lastEdgeClosures = raw closure count`. Confidence → `LATCHED_ENCODER`, provenance `EDGE_CENTRE`. Centre-referencing removes the approach-direction hysteresis without any parking, and every home doubles as a characterisation sample. Exits: swept > 1.25 × ticksPerRev (or 2000 ticks if unknown) with no closure → reverse direction and sweep once more; second miss → `FAILED(NO_EDGE swept=N)`. 12 consecutive no-move pulses → `STALL`. Switch closed continuously for > 60 ticks of travel → `FAILED(SWITCH_STUCK_CLOSED)` (polarity hint). Hard backstop 25 s per sweep. **On any failure the encoder frame is untouched and confidence becomes `STALE` (if it was `LATCHED`) or stays `UNKNOWN`.**
Pass 3 (best-effort settle): if `|deg| > 3°`, one `RotaryMoveToTicks(0)` at min power, bounded to 2 s; any failure is logged `SETTLE SKIPPED (reason)` and ignored. It cannot change confidence and never triggers a re-search.

Logs: `HOME p0 rest-closed=0`, `HOME p1 -> -10deg ok`, `HOME p2 dir=CCW pulse=4..22ms swept=317t enter=312 exit=328 arc=16t(4.9deg) closures=7 zero@centre`, `HOME DONE deg=+2.1 conf=LATCHED`.

### 3.9 RotaryCharacterise(revs 1–10)

Composite prepends `RaiseThenRotate`. Continuous drive at `minPower + 10%`. Per pass with trailing-edge debounce: `enter`, `exit`, raw closures; ticks/rev = successive `enter` deltas. Verdict: `spread% = (max−min)/mean`; confident if `revs ≥ 3 and spread ≤ 2%`, probable if `revs ≥ 2 and spread ≤ 5%`, else not repeatable (EMI/loose encoder). Reports contact arc mean/min/max in ticks and degrees, chatter (closures per detection), creep step vs arc verdict (`arc < 2×step` → warn). Writes `ticksPerRev` (float) back via `PersistQueue` when confident or probable, logging old→new; also sets `contactArcTicks`, `chatterPerPass`. Ends with a fresh `EDGE_CENTRE` zero from the last pass (so the head is `LATCHED` after the test) and a pass-3 settle. Exits: ESTOP, DIAG, no edge within 1.25 rev of the first pass → `FAILED(NO_SWITCH)` with the polarity hint, deadline `revs × 20 s + 10 s`.

### 3.10 RotarySpin (`:PR`) and RotaryJog (rescue)

Same machine, two gates. `Spin`: floor `minPower` (40%), ramped, stall → stop (today's `ROTARY NOT MOVING`), ends on next command; `:PR0` → `RotaryHome`. `Jog`: floor 0.10, no stall exit, keepalive deadline 2 s, allowed only with the supervisor-owned override or the normal policy; the override cannot be pre-empted by any other context because there is no other writer. Both: lifter below safe height (per §4) → refused/aborted.

### 3.11 SafeLower (`:PH[speed]`)

1. Lifter `UNKNOWN` → `LifterSeek(top)`.
2. Lifter `KNOWN` and ≤ safe height → skip homing (the head is inside the opening, therefore aligned; rotary motion has been refused below this height by policy), go to 5. If already at bottom → `DONE(ALREADY_HOME)`.
3. Rotary not home per §4 → `RotaryHome`. Failure → **stop, stay raised**, `FAILED(HOME_FAILED)`; no raise-home-lower retry loop (the old one is a minute of unresponsive spinning on a bad switch). The operator sees a red LED pattern, `fault:"rotaryHome"`, and the rescue page.
4. Lights off (queued to `I2cService`).
5. `LifterSeek(0, speed = max(fMinimumPower, arg))`; bottom limit re-references.
Result `DONE|FAILED(stage)|ABORTED`; one line per stage.

Random mode and the wizard become `Sequencer` scripts issuing these jobs and waiting on results; `SafetyPolicy` still applies to every job they issue, so a wrong script cannot violate an invariant ("aggressiveness affects speeds and timing, never safety").

## 4. "Homed" evaluation policy — HomeConfidence

```
enum class HomeConfidence : uint8_t { UNKNOWN, SWITCH_LIVE, LATCHED_ENCODER, STALE };
enum class HomeProvenance : uint8_t { NONE, EDGE_CENTRE, REST_CONTACT };
```

- **UNKNOWN**: no reference this power cycle. Power cycle always resets to UNKNOWN; the encoder frame is never trusted across a boot.
- **SWITCH_LIVE**: the debounced switch is closed *now* at rest (2 consecutive 1 ms samples, motor off). This is definitive positive evidence: the plunger only conducts within the 2–7-tick core of the cam zone, so a closed read at rest bounds the error to about ±3 ticks. Entering SWITCH_LIVE from UNKNOWN zeros the encoder there (`REST_CONTACT`). It is a transient state that coexists with LATCHED; when the switch opens again the state falls back to whatever the encoder says.
- **LATCHED_ENCODER**: an edge was captured this power cycle (or a rest-contact zero was taken) and the encoder has been continuous since. "At home" = `|deg| ≤ 10°` (`ROTARY_HOME_TOLERANCE_DEG` kept). This is the v3.6.0 trade made explicit: the switch is used for what it is good at (an edge while moving) and the encoder for what it is good at (holding position between references). Measured encoder accuracy is 0.5% per revolution, well inside 10°.
- **STALE**: a latch exists but something happened that could have broken the encoder-to-head relationship: rotary stall, DIAG fault while rotating, encoder implausibility (phantom-tick counter over threshold while idle, or a move whose measured ticks-per-degree disagrees with `ticksPerRev` by > 5%), or a home attempt that failed after pass 1 moved the head. The frame is kept (it is probably right) but no descent is allowed on it; a re-home is cheap because pass 1 knows roughly where to go.

**Why closed-at-rest is trusted and open-at-rest is not.** The evidence is asymmetric: closure requires the set screw to be over the ball's core; the only false positives are wiring/polarity faults, which show up as "closed everywhere" during any sweep (§3.8's stuck-closed exit) and are therefore detectable. An open read at rest is *expected* at true home, because the marginal band only conducts under vibration and the smallest move is larger than the core. Open-at-rest carries zero information and never demotes confidence. This single rule is what removes the "sitting on home, reading open, searching again" failure class.

**Which motions accept which levels:**

| Motion | UNKNOWN | SWITCH_LIVE | LATCHED | STALE |
|---|---|---|---|---|
| Descend below safe height | refused → composite homes first | allowed | allowed if `|deg| ≤ 10°` | refused → re-home |
| Absolute rotate (`:PA`, random) | home first (+ characterise if no ticksPerRev) | allowed | allowed | home first |
| Relative rotate (`:PD`) | allowed | allowed | allowed | allowed |
| Spin / jog | allowed | allowed | allowed | allowed |
| Calibrate lifter | refused (needs home or rotary disabled) | allowed | allowed | refused |

**Degradation and promotion rules:** power cycle → UNKNOWN; rotary stall/DIAG-while-moving/implausible encoder → STALE; motor *disable* (EN low) does not degrade anything — the head cannot be back-driven through its gearing (documented) and the lead screw is self-locking; a failed home never re-zeros; a successful edge or a rest-contact read promotes to LATCHED; a rest-contact read while LATCHED with `|deg| > 10°` corrects the frame to the switch and logs it loudly (the switch wins over the encoder, always). `fDisableRotary` reports `LATCHED` permanently with provenance NONE, as today's `rotaryAtHome()` returns true.

The lifter has the parallel `UNKNOWN/KNOWN/STALE` (§3.4); `rotaryAllowed` requires lifter `KNOWN` above safe height, or the top limit physically closed (which is also accepted while STALE).

## 5. ESTOP semantics

**Recommendation: a one-way latch, deliberately released, with a separate non-latching soft stop.** Today `:PX` is abort-and-continue: the flag is consumed by the first loop that notices and queued chain steps then run. With non-blocking jobs and a queue, abort-and-continue would mean the step after the one you panicked on starts moving a few ms later. That is the opposite of what a panic button is for. Sabé already uses the latch model on the same droid (`CLAUDE.md` invariant 3), so operator expectations across boards match.

- `EstopLatch::trip(source)`: atomic flag set, `panicStop()` (four LEDC zeros), source/time recorded, sequence bumped. Idempotent, callable from any context, never blocks.
- Supervisor on seeing the latch: current job → `ABORTED(ESTOP)`, both axes `Brake` then `Coast`, `enableRequested = false`, `CommandQueue` flushed, `Sequencer` told to drop the chain, cancel `:PW`, end random mode and any wizard script. Position knowledge is preserved (PCNT keeps counting through the brake, and the lead screw/gearing hold the mechanism), so confidence is not degraded by the ESTOP itself.
- While latched: motion requests are refused with `ESTOP LATCHED (source=web at t=…): send :PXR or press Reset`; `:PL`, status, and configuration reads still work.
- Release: `:PXR` on serial (the RX scanner only matches `:PX` followed directly by CR/LF, so `:PXR` cannot re-trip it), `/api/estop?reset=1` on the web, and the mesh equivalent. Release lands in Idle with motors coasting and nothing queued; it never resumes what was interrupted.
- Soft stop (non-latching): `/api/stop`, and `:PS` semantics unchanged. Cancels the job and flushes the queue, no latch. The periscope page's "■ STOP" becomes soft; the rescue page's "EMERGENCY STOP" stays a latch.
- Convergence: `:PX` from either UART's RX scanner (`advanceEstopFsm`, verbatim), `/api/estop`, `?STOP`/`&ESTOP` from the mesh, and a future physical input all call the same `trip()`. Internal faults (DIAG, stall, watchdog) are **not** ESTOPs; they go through `FaultModel` and stop only the affected job.

Trade-off acknowledged: any existing controller binding that sends `:PX` and expects the next command to work will now need the reset action too. The README must say so, and the release can be bound to the same controller.

## 6. Fault model

| Fault | Detection | Effect | Clears | Operator sees |
|---|---|---|---|---|
| `LIFTER_DIAG` / `ROTARY_DIAG` | pin low for 2 consecutive ticks | that axis Brake→Coast; job `FAULT(DIAG)`; `I2cService` cycles EN (low 50 ms, high) to clear a latched OCP; count++ | self-clears when DIAG high ≥ 100 ms after the EN cycle (thermal recovery may take seconds — `FaultWait` inside calibration bounds it at 15 s) | LED yellow/red, `fault:"lifterDiag"`, log `LIFTER DIAG during SEEK UP pos=812 (n=1)` |
| `DIAG_REPEATED` | ≥ 3 DIAG trips in 60 s | latching; motion refused | `clearfault` | as above plus `REPEATED` |
| `STALL` (per axis) | §3 per-machine | job fails; axis knowledge → STALE | next limit contact (lifter) / successful home (rotary); a new job may be issued at once | `STALL SEEK DOWN at 412 after 700ms` |
| `SLOW_PROGRESS` | velocity below profile threshold | warning + one reboost; then STALL | — | `RE-BOOST (slow 8t/s)` |
| `ENCODER_DIRECTION` | ticks against drive direction > 8 | job fails; lifter STALE | manual: check `fLifterInverted`/wiring | loud, with the hint |
| `ENCODER_NOISE` | ticks while both motors off > 5 in 10 s (phantoms) | counter in snapshot; > 20/min → that axis STALE and drift-correction disabled | re-reference | `PHANTOM TICKS lifter=+3 while idle (n=7)` |
| `LIMIT_CONTRADICTION` | top and bottom both closed ≥ 3 ticks | latching while present; all lifter motion refused | clears when it stops; then a re-reference is required | polarity hint |
| `LIMIT_MISMATCH` | limit closes with encoder > 25% of travel away from that end | trust the switch, re-reference, mark travel calibration suspect | — | `LIMIT MISMATCH bottom at 340 ticks: encoder re-referenced` |
| `HOME_SWITCH_STUCK` | closed for > 60 ticks of rotary travel | home fails | next open read | polarity hint |
| `EXPANDER_FAULT` | I2C timeout/NACK on EN write, or enable not acked in 50 ms | job fails; cannot arm | next successful transaction | `EXPANDER I2C FAULT: motors cannot be enabled` |
| `TICK_OVERRUN` | cycle > 800 µs or a missed tick | counters only | — | `/api/status` fields `cycleUsMax`, `lateTicks` |
| `WDT` | supervisor stops petting for 1 s | chip reset; PCF8574 power-on state disables the drivers | boot | boot banner shows reset reason |
| `PERSIST_WHILE_ARMED` | a persist request arrives while armed | write deferred (never refused silently) | on disarm | log `NVS write deferred until motors idle` |

Latching: `DIAG_REPEATED`, `LIMIT_CONTRADICTION` (while present), ESTOP. Everything else self-clears or clears on the next successful reference. All faults are in the snapshot as a bitmask plus the most recent reason string id.

## 7. HAL, simulator, and host tests

### 7.1 HAL interface (header-level)

```cpp
// src/Hal.h — pure, no Arduino headers
#pragma once
#include <cstdint>
namespace uppity {

enum class Axis : uint8_t { Lifter, Rotary };
enum class Bridge : uint8_t { Coast, Brake, Drive };   // Drive uses duty sign for direction

struct BridgeCommand { Bridge mode; int16_t duty; };   // duty -1000..+1000 (0.1 %), 0 with Drive == Brake per §8

struct RawInputs {
    int32_t lifterTicks, rotaryTicks;
    bool topLimit, bottomLimit, homeSwitch;              // already polarity-corrected, raw (undebounced)
    bool lifterDiag, rotaryDiag;                         // true == fault (pin low)
    bool enableAcked;                                     // I2cService shadow
};

class Hal {
  public:
    virtual ~Hal() = default;
    virtual uint32_t nowMs() const = 0;
    virtual uint32_t nowUs() const = 0;
    virtual void readInputs(RawInputs& out) = 0;          // tick-safe: registers/GPIO only
    virtual void writeBridge(Axis a, BridgeCommand c) = 0;// tick-safe: LEDC only, writes on change
    virtual void panicStop() = 0;                         // any context: all four PWMs to zero
    virtual void requestEnable(bool on) = 0;              // tick-safe: sets an atomic; I2cService acts
    virtual void setEncoder(Axis a, int32_t ticks) = 0;   // re-reference (PCNT clear + software offset)
    virtual void setLifterInverted(bool inv) = 0;         // swaps PWM pins and PCNT level actions together
};

// Provided by glue; used by CommandQueue/StatusSnapshot. No-op on host.
struct Lock { virtual void lock() = 0; virtual void unlock() = 0; virtual ~Lock() = default; };

} // namespace uppity
```

`MotionSupervisor` signature: `explicit MotionSupervisor(Hal&, const LifterMotorProfile*&, RotaryCharacterisation&, EstopLatch&, CommandQueue&, SnapshotPublisher&, EventLog&)`; `void begin(uint32_t nowMs)` (runs `BootPositionResolve`); `void tick(uint32_t nowMs)`. Nothing else is public; the web/serial layers talk through the queue and the snapshot.

`Esp32Hal` notes: PCNT per axis — one unit, channel 0 edge = A (count on positive edge, hold on negative), level = B (keep on high, invert on low), `glitch max_glitch_ns = 10000`, limits ±32767 with `accum_count = 1` so `pcnt_unit_get_count` returns a wide accumulated count; the software offset makes `setEncoder` free of clearing races. Create the units from the supervisor task so the overflow ISR lands on core 1. `pcnt` conflicts with the legacy driver used by `ESP32Encoder`; do not link that library.

### 7.2 Mechanism simulator (`src/sim/`)

Deterministic, seeded, parameterised. Advanced by `SimHal::advance(1 ms)` with the last bridge command applied.

`LifterModel` parameters: `breakawayDuty` (static friction; motion starts only after the drive has been ≥ breakaway for `breakawayDwellMs`), `kv` (ticks/ms per unit duty above running friction), `runningFriction`, `coastDecel` and `brakeDecel` (ticks/ms² when Coast vs Brake), `travelTicks`, limit windows (bottom ≤ 0, top ≥ travel, with 2-tick hysteresis, optional bounce), `starvationWindowMs` (average duty over the window below breakaway → no motion: models the 19:1 buzz), `phantomRatePerMs` while driving and while idle (random ±1 ticks), DIAG injection (`diagLowFor(ms)`, thermal trip after `hotMs` at duty ≥ `hotDuty`), `encoderFrozen` (motor moves, count does not), `inverted`. Boot state settable (position, which limits closed).

`RotaryModel`: `ticksPerRev` (float, e.g. 1184.6), same friction/momentum with **stick-slip**: a drive pulse shorter than `minPulseMs` produces nothing, longer produces a burst quantised to `[minBurstTicks, maxBurstTicks]` (5–20); spherical-cam switch: centre `c`, `halfWidthMoving` (8 ticks), `halfWidthRest` (2 ticks) applied when |velocity| < `restVelocity`; in the marginal band (between rest and moving widths) the contact toggles pseudo-randomly each ms with `chatterProbability` tuned to ≈ 7 make/break per pass; first-contact edge is direction-dependent purely from geometry; `stuckClosed`, `disconnected`, `polarityWrong` switches; phantom ticks; DIAG injection.

`SimHal`: owns both models, applies limit polarity, models the expander (`enableLatencyUs` before `enableAcked`, `failEnable`), counts I2C-equivalent operations by context so a test can assert the tick made none, records every bridge command with its timestamp (so tests can assert "PWM zero within N ms of trip" and "no drive after latch").

`test/native/`: `Makefile` (`c++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined`, sources from `../../src`), `uppity_test.h` (the `TEST/CHECK/CHECK_EQ` harness from Roam-A-Dome's `rad_test.h`), `test_main.cpp`, and a `Rig` helper that wires `SimHal + MotionSupervisor + CommandQueue + Sequencer` with a fake clock and `runMs(n, callback)`.

### 7.3 Host tests that would have caught the known failures

September 2026 homing failure and the "parks at 66%" symptom:
1. `home_finds_edge_from_every_start_angle` — 13 start angles including on-switch and 64° off a corrupted frame; adaptive creep succeeds each time within 1.25 rev; the same rig with a fixed 3/1 ms pulse fails (stick-slip model), which pins the mechanism.
2. `home_search_bound_is_distance_not_time` — slow drivetrain (20-tick bursts, 1191 t/rev): failure is declared only after ≥ 1.25 rev swept, never at 10 s.
3. `home_zero_is_centred_for_both_approach_directions` — CW and CCW homes agree within 2 ticks.
4. `chatter_produces_exactly_one_edge_event_per_pass` — 7 make/break per pass, 3 passes → 3 events; characterise reports ticks/rev without the `{0,0,1186}` artefact.
5. `failed_home_never_rezeroes` — switch disconnected: encoder frame identical before/after, confidence not LATCHED, result FAILED, log says so.
6. `resting_open_read_does_not_block_descent` — head parked 5 ticks off-centre reading open after a good edge → `SafeLower` reaches the bottom limit.
7. `unhomed_droid_stops_at_safe_height_and_says_why` — homing fails → lifter stays at the safe height, result `FAILED(HOME_FAILED)`, snapshot `fault` set; no silent `DONE`.
8. `settle_failure_keeps_the_home` — pass-3 stall after a good edge: confidence still LATCHED, no re-search issued.
9. `rest_contact_corrects_a_wrong_frame_loudly` — LATCHED with 40° error, switch closes at rest → frame corrected, event emitted.

Boot at zero:
10. `boot_with_mast_up_references_from_top_limit` — top=1 → KNOWN at travel; rotary allowed; no motion at boot.
11. `boot_mid_travel_permits_only_up_until_referenced` — down refused with `LIFTER_UNKNOWN`; up seek references at the top; down then allowed.
12. `boot_with_both_limits_closed_is_a_fault`.

ESTOP unreachable during the wizard, and semantics:
13. `estop_from_web_context_stops_calibration_within_2ms` — trip from a second "context" mid-sweep; PWM zero within 2 ticks; supervisor never re-drives; job ABORTED.
14. `estop_latches_and_flushes_chain_and_random_mode` — `:PP100:PW2:PP0` then trip: queued steps dropped, `:PP` refused until `:PXR`, `:PL` still works.
15. `rx_scanner_catches_px_mid_chain` — byte-feed `:PX\r` through `advanceEstopFsm` while a seek runs.

Rescue-override race:
16. `rescue_jog_is_not_cancelled_by_policy` — override on, lifter below safe height, rotary jog at 10%: runs for its keepalive window through 100 policy evaluations; stops only at the deadline.
17. `jog_floors_web_10_serial_40`.

`:PD` multi-revolution bug:
18. `pd_720_and_minus_720_rotate_two_full_revolutions` and `pd_speed_argument_is_honoured`.
19. `pa_takes_shortest_path_and_pa_after_pd720_lands_within_tolerance`.

Mechanism-specific behaviour and structure:
20. `seek_pulses_in_cruise_and_limit_impact_velocity_is_bounded` (continuous drive in the same rig exceeds the bound).
21. `wizard_creep_is_continuous_and_moves_a_19_to_1` (starvation model: pulsed would not move).
22. `breakaway_then_ramp_then_reboost_on_slow_progress`; `stall_timeout_follows_profile` (700 vs 2000 ms).
23. `calibration_travel_requires_repeatability_not_magnitude` — 8% spread rejected, stored kept; a 250-tick rotary and a 450-tick lifter accepted.
24. `characterise_writes_ticks_per_rev_back_when_confident` — 1191 stored, 1185 measured spread 1 → written; spread 8% → not written.
25. `diag_fault_stops_motor_and_recovers_after_en_cycle`; `calibration_fault_wait_is_bounded_at_15s`.
26. `nvs_write_requested_mid_seek_is_deferred_until_disarmed`.
27. `tick_makes_no_i2c_calls` and `enable_is_written_only_on_change`.
28. `snapshot_seqlock_never_yields_a_torn_read` (interleaved publish/read).
29. `home_edge_events_are_not_aliased` — 3 passes at 2 s spacing → 3 events with distinct timestamps (the old 2 s throttle would have produced 1).

## 8. Risks, trade-offs, and what to keep

**PCNT on Arduino core 3.x.** Available: `driver/pulse_cnt.h` is in the installed IDF 5.5 libs for the classic ESP32 (8 units, 2 channels each, glitch filter). Not wrapped by the Arduino core, so the glue calls IDF directly. Risks: the legacy `driver/pcnt.h` cannot coexist (do not link `ESP32Encoder`); the hardware glitch filter caps at 1023 APB cycles ≈ 12.7 µs, so set 10 µs — encoder edges at full speed are ≥ 200 µs apart on both axes, so nothing real is filtered; the counter is 16-bit with driver-side accumulation, which the `accum_count` flag handles. Fallback if PCNT misbehaves on a board: the HAL can be swapped for a GPIO-ISR implementation without touching the core, but phantom-tick immunity is then software-only.

**Brake vs coast truth table — verify on the datasheet.** The sketch and `pin-map.h` treat PWM1=PWM2=low as coast and both-high as brake. The TB9051FTG's output logic, as I recall it, makes both-low a low-side short (brake) and both-high a high-side short, with true high-Z only when the driver is disabled via EN/ENB. If that is right, today's "off" period is already a brake, which is why pulsing decelerates the screw so well, and true coast costs an I2C transaction. The design does not depend on which is true — `Bridge::Coast/Brake` map to pin states in one line of `Esp32Hal` — but the simulator's `coastDecel/brakeDecel` and the choice of off-period mode should be set once this is confirmed. Todd can settle it in a minute with the datasheet.

**20 kHz carrier changes calibrated floors.** Min-power values found at 1 kHz will not carry over exactly. The upgrade path runs §3.3 P2 (min-power discovery) automatically on first commissioning; profiles' default floors remain as starting points. If a builder's motor or driver runs hot at 20 kHz, 10 kHz is the documented fallback; do not go back to 1 kHz.

**Flash writes stall both cores.** Enforced, not advised: the only NVS path is `Persist`, gated on the snapshot's `disarmed`, with `persistBusy` blocking arming during the commit. OTA start refused while armed. The old `EEPROMSettings::write()` commits the whole blob; keep that behaviour but through the queue. Also: `sLifterParameters.save()` calls scattered through the web handlers are replaced by `PersistQueue::request(...)`.

**Tick jitter.** Expected small; measured via `cycleUsMax`/`lateTicks`. If a future feature adds a core-1 task above priority 10, that is the number to watch. Keep `Serial` and `String` out of the supervisor task entirely (the event ring exists for this).

**I2C service latency for the SD-card variant.** Limit switches on the expander arrive with 5–20 ms latency. The HAL supports it (limit bits from the `I2cService` shadow), but that variant must run with larger soft-approach zones and lower cruise speeds; make it a profile flag (`limitsOnExpander`) that widens `rampZone` and caps `speed`, and say in the README that the standard board is the tuned target.

**Latch changes `:PX` UX.** Covered in §5; document and provide `:PXR`.

**Rotary uncharacterised on first use.** Absolute moves need `ticksPerRev`; `FirstUse` runs a two-revolution characterisation the first time (this is what the old safety maneuver's one-revolution measurement did, done better). It requires the lifter above safe height, so first use of the rotary always raises first — same as today.

**Flash headroom.** 94% today with ReelTwo's web/SMQ stack. The pure core is small (no `String`, fixed-size rings), and dropping `TargetSteering`/PID and the dead output-limit machinery pays for it; but `WebPages.h` is the elephant and is out of this document's scope.

**Behavioural changes to flag to builders:** ESTOP latch; `:PH` no longer retries raise-home-lower; `:PD` now really rotates; calibration produces different data and must be re-run; boot no longer runs a safety maneuver on first motion (position comes from the limits).

**Keep verbatim or near-verbatim:** `advanceEstopFsm` and the RX-callback placement (1085–1125); `SerialFifo` (1037); the per-line ':'/'#' acceptance rule and separate command-serial line buffer (6718–6752); the three `LifterMotorProfile` tables and their rationale comments (293–366); `rotaryHomeSelfTest`'s trailing-edge debounce and reporting text (2662–2849) as the basis of `RotaryCharacterise`; the breakaway three-band ramp design (1596–1619) as the `Ramp` spec; the two-flip overshoot hysteresis (2217–2230); the "fall back to the highest calibrated slot" lookup policy (4216–4265); the `rotaryAtHome()` comment block (1337–1353) as the README text for §4; `WebLogBuffer`; the status-LED colour table; `CircularMath.h` from Roam-A-Dome; the `rad_test.h` harness and Sabé's `Preferences.h` shim (failure injection) for the persist tests.

### Critical Files for Implementation

- `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/R2UppitySpinnerV3.ino` — reference behaviour: profiles (256–371), ESTOP scanner (1085–1125), seek/breakaway (1533–1995), rotary homing/creep/self-test (2299–2849), calibrate (3471–3828), ISRs (4649–4692), loop/dispatch (6546–6950)
- `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/docs/REWRITE-HANDOFF.md` — requirements and the hardware facts every state machine encodes
- `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/WebPages.h` — the core-0 handlers (2183–2305, 2595–2669) that must become enqueue-only
- `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Roam-A-Dome/Roam-A-Dome_ALT/RadFirmware/src/MotionController.h` and `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Roam-A-Dome/Roam-A-Dome_ALT/RadFirmware/test/native/rad_test.h` — the pure-core and host-test style to follow
- `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Control/Amidala Claude Project/Sabe/SabeFirmware/SabeControl.cpp` (160–235) and `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Control/Amidala Claude Project/Sabe/test/shims/Preferences.h` — the proven control-task idiom on esp32 core 3.3.4 and the NVS failure-injection shim
