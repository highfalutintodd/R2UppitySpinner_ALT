# v3.6.0 firmware inventory

Generated 2026-09-07 by an exploration pass over `R2UppitySpinnerV3.ino`, `pin-map.h`, `Screens.h`, `menus/` at v3.6.0 (commit 18ce9f3). Line numbers refer to that revision. Kept as the reference inventory for the v4 rewrite so the old code never has to be re-read.


# R2 Uppity Spinner V3 (ALT fork) — complete firmware feature & behaviour inventory

Firmware `v3.6.0`, `R2UppitySpinnerV3.ino` 6952 lines. All paths relative to
`/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/`.

Note up front: `docs/REWRITE-HANDOFF.md` already covers the rotary/homing history and the known-bug list well. This report is the *code-level* inventory that document points at, and it adds several defects the handoff does not mention — most importantly **stored sequences are broken past their first element** (§7.9) and **network OTA is never started** (§12).

---

## 1. ReelTwo library dependencies (and non-ReelTwo deps)

### 1.1 ReelTwo headers actually included (`.ino:439–686`)

| Include | Line | What the sketch uses it for | Depth of weave |
|---|---|---|---|
| `ReelTwoSMQ32.h` | 439 | Pulled in **instead of** `ReelTwo.h` when `USE_DROID_REMOTE` is set. Defines `USE_SMQ` + `USE_SMQ32`, provides ESP-NOW-based `SMQ::` API and `SMQMESSAGE()`. | Deep — the whole remote/menu subsystem is behind it |
| `ReelTwo.h` | 441 | Fallback when remote is disabled. Supplies `SizeOfArray`, `REELTWO_READY()`, `PrintReelTwoInfo()`, `IRAM_ATTR`, `DEBUG_*` macros | Shallow but pervasive (macros) |
| `core/SetupEvent.h` | 443 | `PeriscopeLifter : public SetupEvent`; `SetupEvent::ready()` at `.ino:6189` drives all pin setup | Structural — one call site, easy to replace |
| `core/AnimatedEvent.h` | 444 | `AnimatedEvent::process()` at `.ino:6625`. **The lifter is NOT an AnimatedEvent** — `lifter.animate()` is called explicitly at 6626. The registered AnimatedEvents are `statusLED`, `wifiAccess`, and `wifiMarcduinoReceiver` | Shallow |
| `core/AnalogWrite.h` | 445 (**commented out**) | Not used — `::analogWrite()` is the ESP32 Arduino core's LEDC wrapper | none |
| `encoder/PPMReader.h` | 446 | `PPMReader sPPM(PIN_PPMIN_RC, 6)` at 1161 for RC input. **Effectively dead** (§14) | Trivial |
| `core/PinManager.h` | 579 | Base class for `CustomPinManager` (593–645); all GPIO goes through `sPinManager.digitalRead/Write/pinMode` so pins ≥ 200 are transparently routed to the PCF8574 | Deep — every pin access |
| `core/SingleStatusLED.h` | 657 | `SingleStatusLED<PIN_STATUSLED>` = one-pixel NeoPixel status LED with a 4-frame colour table | Moderate |
| `drive/TargetSteering.h` | 685 | PID wrapper (`drive/PID.h`) used in `seekToPosition` (1690), `seekToTop` (4421), `calibrate` (3551, 3708). Note `seekToPosition` deliberately sets `limit = 0` (1708, 1851) so the PID is **constructed and configured but never actually drives throttle** in normal seeks | Deep in calibration, vestigial in seeks |
| `core/EEPROMSettings.h` | 686 | `EEPROMSettings<LifterSettings> sSettings` (917). Provides `read/write`, CRC + magic validation, and the **stored-sequence store** (`readCommand`, `writeCommand`, `deleteCommand`, `clearCommands`, `getCommands`, `listSortedCommands`) | Deep |
| `wifi/WifiAccess.h` | 458 | `WifiAccess wifiAccess` (4742); AP/STA connect state machine; it is an `AnimatedEvent`, so **WiFi association runs on core 1 from `loop()`** | Moderate |
| `wifi/WifiWebServer.h` | 461 | `WifiWebServer<20, SizeOfArray(pages)> webServer(pages, wifiAccess)` — `WebPages.h:2713`, port 80, 20 client slots. `WPage`/`WAPI`/`WUpload`/`WElement` DSL. `handle()` runs on **core 0** | Deep (whole web UI) |
| `wifi/WifiMarcduinoReceiver.h` | 464 | `WifiMarcduinoReceiver wifiMarcduinoReceiver(wifiAccess)` (4753), TCP port 2000, handler at 6308 prints `[JAWALITE] %s` and calls `executeCommand(cmd)`. It is an `AnimatedEvent` → runs on core 1 | Shallow |
| `menus/CommandScreen.h`, `menus/CommandScreenHandlerSMQ.h`, `menus/utility/{ChoiceIntArrayScreen,ChoiceStrArrayScreen,UnsignedValueScreen,MenuScreen}.h` | 6055–6063 | Droid Remote menu framework; `CommandScreenHandlerSMQ sDisplay` (6058) renders the remote's OLED over SMQ | Moderate, isolated to `menus/` |

### 1.2 ReelTwo things the parent asked about that are **NOT** used
`WifiSerialBridge` (no serial-over-TCP), `core/Marcduino.h`, `core/JawaCommander.h` / `core/JawaEvent.h` (no JawaLite parser — the only "JawaLite" is a printf label), `core/CommandEvent.h`, `core/Animation.h` / AnimationPlayer, `core/DelayCall.h`, `core/PersistentStorage.h`, `ServoDispatch*`, any `motor/` or `drive/Tank*`/`Dome*` driver, `i2c/PeriscopeI2C.h`, `bt/PSController`. Marcduino parsing is entirely hand-rolled in this sketch (`processCommand` / `processLifterCommand` / `processConfigureCommand`).

### 1.3 Debug-macro override trick (`.ino:54–86`, 449–455, 480–564)
`DEBUG_PRINT`/`DEBUG_PRINTLN` are **pre-declared before** `ReelTwo.h`, then `#undef`'d and re-defined **after** it (452–455) so ReelTwo's inline headers (WifiWebServer, WifiMarcduinoReceiver) route through `_debugPrint*`. Those tee to `Serial` **and** `webLogBuffer`, and suppress the library's noisy `"New client:"` lines via `sDebugSuppressNewClient` (537–544). Any rewrite that keeps the web log needs an equivalent hook.

### 1.4 Non-ReelTwo dependencies
- `Adafruit_NeoPixel` — indirectly, via `USE_LEDLIB 1` in `ReelTwo.h:17` → `SingleStatusLEDPCB`.
- `PCF8574` (reeltwo fork), `.ino:582`, address `0x20` (584), interrupt-driven via `flagDigitalReadAll` ISR (588).
- `Preferences.h` (470) — NVS namespace `"uppityspinner"` (6107).
- `Wire.h` (447), `Wire.begin()` at 6188.
- `WiFi.h`/`WiFiAP.h`/`esp_wifi.h` (via WifiAccess), `ESPmDNS.h` (459), `ArduinoOTA.h` (468), `Update` (ESP32 core, used by SD flash 6123 and web `/upload/firmware`).
- `EEPROM.h` (via EEPROMSettings).
- `esp_now.h` (via ReelTwoSMQ32).
- `FS.h` + `SD.h` only when `USE_SDCARD` (567–568).
- FreeRTOS `xTaskCreatePinnedToCore` / `vTaskDelay`.

---

## 2. Task / core structure

### 2.1 `setup()` — `.ino:6099–6395`
1. `REELTWO_READY()` → `Serial.begin(115200)` (macro overridden at line 81).
2. `Serial.onReceive(onUsbSerialReceive)` (6105) — registered immediately so the `:PX` scanner is live during boot.
3. `preferences.begin("uppityspinner", false)` (6107).
4. `USE_SDCARD` only: mount SD, look for `/UPPITY.BIN`, flash it via `Update`, delete it, reboot (6111–6144).
5. `sLifterParameters.load()`; `setMotorProfile()`; restore `sRotaryCircleEncoderCount` from NVS if ≥ `ROTARY_MIN_VALID_CIRCLE_ENC` (6145–6151).
6. Read `wifiEnabled` / `remoteEnabled` from NVS (6154–6158).
7. `PrintReelTwoInfo(Serial, "Uppity Spinner")` (6160) — boot banner.
8. `EEPROM.begin(EEPROM_SIZE)` then `sSettings.read()` (6162–6181) → prints `Settings Restored` / `First Time Settings`. **See defect §14.3.**
9. `initWizardStateFromBoot()` (6186).
10. `Wire.begin()`; `SetupEvent::ready()` (6188–6189) → `PeriscopeLifter::setup()` configures all pins and attaches both encoder ISRs.
11. `COMMAND_SERIAL.begin(sSettings.fBaudRate, SERIAL_8N1, PIN_RXD2, PIN_TXD2)` + `onReceive(onCmdSerialReceive)` (6192–6193).
12. `lifter.disableMotors()`; print `R2UppitySpinner v<ver>` + `READY` (6196–6198).
13. SMQ/remote init incl. pairing/discovery/lost callbacks (6200–6293).
14. WiFi credentials, Marcduino-over-WiFi handler, `notifyWifiConnected` callback (mDNS + "Connect to http://..."), OTA callbacks (6294–6371).
15. `webServer.setConnect()` (6375).
16. `xTaskCreatePinnedToCore(eventLoopTask, "Events", 5000, NULL, priority 1, &eventTask, core 0)` (6381–6388).
17. `if (sRCMode) sPPM.begin();` — always false here (§14.1).
18. `sCurrentInputValue = readInputHeaders()` (6394).

**No safety maneuver runs at boot.** It is lazy — first motion command calls `ensureSafetyManeuver()`.

### 2.2 `loop()` — core 1 (Arduino loopTask), `.ino:6546–6950`
In order each iteration:
1. 60 s heartbeat (6550–6600).
2. `sUpdateSettings` → `updateSettings()` → `sSettings.write()` (6602).
3. Rescue-override 60 s expiry (6607–6611).
4. Jog watchdogs: stop lifter/rotary if `sLifterJogExpiry` / `sRotaryJogExpiry` elapsed (6613–6623).
5. `AnimatedEvent::process()` (6625) → statusLED, wifiAccess, wifiMarcduinoReceiver.
6. `lifter.animate()` (6626) → `rotaryMotorUpdate()` + random-mode dispatch.
7. Drift correction block, 5 s cadence, ≤3 consecutive failures (6632–6688).
8. Drain **one byte** from `sUsbFifo` (6692–6704).
9. Drain **one byte** from `sCmdFifo` with per-line accept filtering (6718–6752).
10. Command dispatch: `sCmdBuffer` chain first, else `sBuffer` chain, else one-shot (6754–6820).
11. Idle motor power-down (6822–6830).
12. Rotary-home edge log + status LED mode (6831–6853).
13. PCF8574 `digitalReadAll` change detect → external trigger → `:PS<n>` (6854–6876).
14. RC/PPM height mapping (6877–6903).
15. Verbose-debug state dump (6920–6949).

### 2.3 `eventLoopTask()` — core 0, `.ino:6400–6430`
`ArduinoOTA.handle()` → `webServer.handle()` → `SMQ::process()` → `sDisplay.process()` (menus) → `vTaskDelay(1)`.

**This is the second motor authority.** Web handlers execute inline inside `webServer.handle()` on core 0 and directly call blocking motion: `lifter.calibrate()` (`WebPages.h:2617`), `lifter.wizardCreepToLimit()` (2603), `lifter.seekToPosition()` (2661), `lifter.rotateHome()` (2290), `lifter.rotaryMotorSpeedForce()` (2200), `lifter.lifterMotorMove()` (2237), `executeCommand("#PSC")` from the Calibrate page (`WebPages.h:1338`). The remote's menu screens also `executeCommand()` from core 0.

### 2.4 Cross-core shared state — **no mutex, no critical section anywhere**
| Symbol | Decl | Writers | Readers |
|---|---|---|---|
| `sWebAbort` | 1008 | UART RX callbacks (UART driver task), `/api/estop` (core 0), `serialAbort()` (core 1), `:PX` handler | every motion loop (core 1 **and** core 0) |
| `sRescueOverrideExpiry` | 1009 | `/api/action` core 0; cleared in `loop()` core 1 | `/api/lift`, `/api/rotary`, `/api/status` |
| `sSafetyManeuver` / `sSafetyManeuverFailed` | 1005–1006 | core 1 maneuver; core 0 `/api/action` and `/api/lift` (which sets `sSafetyManeuver = true` outright, `WebPages.h:2233`) | both |
| `sCalibrating` | 1004 | `calibrate()`, `serialAbort()`, `/api/estop` | nested calibration loops |
| `sRotaryHomed` | 1007 | `rotateLeftHome`/`rotateRightHome`/`setRotaryHome` | `rotaryAtHome()` |
| `sRotaryCircleEncoderCount` | 1013 | safety maneuver (core 1), `clearRotaryEncoderCount` (core 0) | angle math on both |
| `sWizardMotionOverride` | 824 | core 0 only | `lifterMotorMove` on both |
| `sLifterJogExpiry` / `sRotaryJogExpiry` | 6008–6009 | core 0 sets, core 1 clears | both |
| `sUpdateSettings` | 6002 | core 0 | core 1 |
| `sDigitalReadAll` | 587 | PCF8574 ISR | `loop()` |
| `sBuffer`/`sPos`/`sCmdBuffer`/`sCopyBuffer`/`sProcessing` | 1019–1027 | `executeCommand()` from core 0 (web, remote menus) **and** core 1 serial drain | dispatcher on core 1 |
| encoder tick counters | 4605–4617 | ISRs | everything |

`executeCommand()` (1145) writing `sBuffer`/`sPos` from core 0 while core 1 is mid-line is an unsynchronised race.

### 2.5 ISRs and timers
- `PeriscopeLifter::measureLifterEncoder` (4649) — `attachInterrupt(PIN_LIFTER_ENCODER_A, …, CHANGE)` (2921). Fires on **both** edges but only counts on the **LOW→HIGH** transition of A, sampling B for direction; inverts direction when `sSettings.fLifterInverted` (4658).
- `PeriscopeLifter::measureRotaryEncoder` (4668) — same arrangement on `PIN_ROTARY_ENCODER_A` (2924). **One edge only, no quadrature decoding** — the arrangement the handoff doc flags as maximally EMI-sensitive. The in-ISR home-switch stop is **commented out** (4679–4689) because I2C reads are not ISR-safe, which makes `encoder_rotary_stop_limit` (set at 2392/3285) and `encoder_rotary_stop_ticks` dead state.
- `flagDigitalReadAll` (588) — `IRAM_ATTR`, on `PIN_GPIO_INTERRUPT` (17) via the PCF8574 constructor.
- Atomic reads use `cli()`/`sei()` (1291, 1374, 4171–4207) — on ESP32 these are `portDISABLE_INTERRUPTS`-ish macros, not FreeRTOS-aware; they do **not** protect against the other core.
- No hardware timers. All timing is `millis()` + `delay()`.

---

## 3. Motion control — lifter

### 3.1 `seekToPosition(pos, speed)` — `.ino:1654–1995` (the main entry point)
1. `ensureSafetyManeuver()` gate (1656).
2. Clamp `pos` to 0.0–1.0 (1660).
3. **Descend clamp**: if `isRotarySpinning() || !rotaryAtHome()`, floor `pos` at `minSafePos = fRotaryMinHeight / getLifterDistance()` (1661–1666). *This is the clamp responsible for "parks at 66%".*
4. Floor `speed` at `sSettings.fMinimumPower/100` (1670).
5. `target_ticks = pos * getLifterDistance()`; `distance = |target-current|` using `getLifterPositionClamped()`.
6. **Skip trivial seeks**: `distance < 5 && 0.01 < pos < 0.99` → return true (1681). EMI phantom-tick suppression.
7. `approachPower = LIFTER_SEEKBOTTTOM_POWER/100` (1689) — the soft-approach floor for **both** directions.
8. `TargetSteering` constructed with `setDistanceTunings(1.0, minpower, minpower)` (1690–1693) — then **`limit` is forced to 0** (1708/1851) so the PID never runs. Dead machinery.
9. **Calibration gate**: `getUpOutputLimit`/`getDownOutputLimit` must return true or the seek aborts with `SEEK UP/DOWN ABORT: uncalibrated speed=` (1700, 1845). An uncalibrated droid cannot `seekToPosition` at all.

**Seek up loop (1727–1829):**
- `breakawayBoost(speed, distance)` first (1715).
- Exit on `topLimit || (!atFullTravel && reachedTarget) || serialAbort()`. For `pos == 1.0` (`atFullTravel`) the encoder target is **ignored** — only the physical top limit or the stall timeout ends it (1736).
- 500 ms progress log `UP pos=… top=… home=… liftFault=` (1740–1748).
- **Stall**: no encoder change for `sMotor->fStallTimeoutMs` → `SEEK UP ABORT: stall at pos=` (1755).
- **Slow progress**: sampled every 1000 ms; if `ticksPerSec < fSlowProgressMinTicksPerSec` and >20 ticks remain, accumulate; at `fSlowProgressMaxMs` abort. First occurrence instead triggers one `RE-BOOST` (another `breakawayBoost`) (1763–1796).
- **Soft approach** (only when `fSoftApproachEnabled`): `rampZone = max(30, distance * fSoftApproachPct/100)`; inside it `power = approachPower + ratio*(speed-approachPower)` (1814–1823).
- **Pulsed drive every iteration**: `move(power); delay(fPulseOnMs); stop(); delay(fPulseOffMs)` (1824–1827).
- On `topLimit`: `delay(100)` then `resetLifterPositionTop()` → encoder := `getLifterDistance()` (1833–1835).

**Seek down loop (1869–1957):** mirror image, except:
- The down ramp is **unconditional** (not gated on `fSoftApproachEnabled`): `rampZone = max(30, (current - target)/5)` = last 20 % of the commanded move (1945).
- Exit on `botLimit || reachedTarget || serialAbort()` — there is no `atFullTravel` equivalent, so `pos = 0` stops on whichever comes first.
- On `botLimit`: `resetLifterPosition()`, `delay(100)`, `resetLifterPosition()` again, and warn if still non-zero (1961–1969).

**On success (1979–1993):** `pos <= 0.01` → clear hold; otherwise `fLifterTargetPos = pos; fLifterHolding = true`. `lifterMotorStop()` (coast) in both branches — `lifterMotorBrake()` exists (1506) but **is never called**, so "holding" is purely the drift-correction re-seek, not a brake.

### 3.2 `breakawayBoost(power, seekDistance, burstTicks=10, maxMs=0)` — 1533–1623
- `maxMs = 0` → `sMotor->fBreakawayMaxMs`.
- `seekDistance in (0,30)` → skipped entirely (1544).
- `rampTicks = clamp(min(fBreakawayRampTicksCap, seekDistance/3), ≥20)` (1546–1552).
- **Phase 1** continuous drive until `burstTicks` of movement or `maxMs`, checking only the limit *in the direction of travel* (1554–1573). Failure prints `BREAKAWAY timeout (N ticks)` and returns false — **but every caller ignores the return value.**
- **Phase 2** ramp into pulsed: 0–75 % of `rampTicks` = `fPulseOnMs` on with no stop; 75–90 % adds `lifterMotorStop()`; 90–100 % adds `delay(fPulseOffMs)` (1596–1619).
- Prints `BREAKAWAY after Nms ramp=N ticks`.

### 3.3 Other lifter movers
- `seekToTop(speed, usePID=true, updateDistance=true)` — 4332. Non-PID branch: `mpower = fMinimumPower/100 + 0.05 + 0.1*speed` (4336, note `speed` is a *fraction*, so this is a near-constant ~min+5–13 %), breakaway, pulsed drive, position-change stall. On top limit it performs **distance validation** (§3.5). PID branch (4414+) requires `isRotaryAtRest()` and uses `TargetSteering` with the calibrated limit.
- `seekToBottom(usePID)` — 2988. `usePID=false` is what everything uses: requires `isRotaryAtRest()` (2990), power `LIFTER_SEEKBOTTTOM_POWER/100`, breakaway, pulsed, stall timeout, then on bottom limit sets `fLifterDistance` if it was 0 and double-`resetLifterPosition()`. The `usePID=true` branch (3050–3099) uses a raw `PID<float>` and is effectively unused.
- `seekToPositionSlow` — 4464. **Dead code**, never called.
- `wizardCreepToLimit(goDown)` — 3404. Wizard-only; **continuous drive, no pulsing** (comment at 3424 explicitly says pulsing chokes creep); `creepSpeed = clamp(max(0.30*fMaxCommandedThrottle, fDefaultMinPower/100), ≤ fMaxCommandedThrottle)`; 1500 ms stall, 30 s ceiling; returns 1/0/-1.

### 3.4 Speed → throttle mapping and floors
- `lifterMotorMove(throttle)` (1446) is the **single chokepoint**: wizard gate → invert → `throttle = clamp(|throttle|, 0, effectiveMaxThrottle())` → `< 0.10` becomes 0 → PWM written only if changed (`fLifterThrottle != throttle`).
- `dualAnalogWrite` (1436) writes `|v| * 255` to the LEDC channel; direction is chosen by which of PWM1/PWM2 gets the value.
- `effectiveMaxThrottle()` (793) = 1.0 if Expert Mode **and** motor is 19:1, else `sMotor->fMaxCommandedThrottle`.
- Min-power sources, in layers: `sSettings.fMinimumPower` (calibrated, floor in `seekToPosition`), `sLifterParameters.fLifterMinPower` (user param, used as the calibration sweep start), `LIFTER_SEEKBOTTTOM_POWER` (approach floor + `seekToBottom` power).

### 3.5 Calibration sweep — `calibrate()` 3471–3828
1. `sCalibrating = true`; `fMinimumPower = max(fMinimumPower, LIFTER_MINIMUM_POWER)`; run `safetyManeuver()` or abort (3475–3482).
2. `retry:` label — max 3 retries if `!isRotaryAtRest()` (3483–3499).
3. Wipe both limit tables, clear `fUpLimitsCalibrated`/`fDownLimitsCalibrated`, `fMinimumPower = 0`. Deliberately **does not** reset `fLifterDistance` (3500–3509).
4. **Min-power discovery**: step `topSpeed` from `LIFTER_MINIMUM_POWER` by 5 to `sweepCap`, driving 400 ms per level, break on top limit or on `lifterStatus.isMoving()` (3514–3527). Then `seekToBottom(false)`.
5. `targetDistance = sSettings.getLifterDistance()` (3530).
6. **UP sweep** (3533–3650): for each 5 % step, `outputLimit = max(topSpeed*3.1, 10)`; up to `CALIBRATION_MAX_TRIES = 4` attempts. Each attempt: `seekToBottom(false)`, `delay(1000)`, run a `TargetSteering` seek to `targetDistance`, exit on top limit / `encoder >= targetDistance` / stall (`fStallTimeoutMs`). No movement → drop `outputLimit` to 0 and retry at full throttle; success (`topLimit || maxEncoderVal >= targetDistance-5`) → record `fUpLimits[topSpeed/5]` and set `fMinimumPower` if still 0; otherwise `reduction = max((target-encoder)*2, outputLimit/2)` and retry. Cooldown `sMotor->fCalibrationCooldownMs` between levels.
7. `fUpLimitsCalibrated = true`; `seekToBottom(false)`; **thermal-fault wait**: poll `lifterMotorFault()` up to 15 s, re-asserting `enableMotors()` every 500 ms; abort down-calibration if it never clears (3667–3687).
8. **DOWN sweep** (3689–3795): starts at `fMinimumPower`, skips speeds with no up-limit, `outputLimit = max(limit*1.5, 160)`; each attempt does `seekToTop(fSafetyManeuverSpeed, usePID=false)` first, then a steered descent; on bottom limit `resetLifterPosition()` and record `fDownLimits[topSpeed/5]`.
9. `seekToBottom(false)`; `fDownLimitsCalibrated = true`; `sSettings.write()`; print `SUCCESS` plus a **recommended min speed = `fMinimumPower + 5`** and a nudge toward the Parameters page / `#PMINPWR` (3805–3825). **`#PMINPWR` does not exist** (§14.6).
- Sweep cap: `effectiveCalibrationSweepCap()` (800) = 100 if Expert+19:1, else `fCalibrationMaxSweepPct`.
- Abort: any `serialAbort()` or `!sCalibrating` in any nested loop.

### 3.6 Travel-distance validation (the "20 % rejection", actually 10 %)
In `seekToTop` non-PID on top-limit hit (4370–4405), with `updateDistance == true`:
- `upCount < 50` → `LIFTER DISTANCE REJECTED (too small)`, keep stored.
- `stored >= 100 && |upCount - stored| > stored/10` → `LIFTER DISTANCE REJECTED (>10% from stored)`, use stored. (README §Troubleshooting says 20 % — the code says **10 %**.)
- else accept: `sSettings.fLifterDistance = upCount`.
With `updateDistance == false` (the safety maneuver, 3217) it prints `LIFTER DISTANCE (safety, not updating)` and forces `sSettings.fLifterDistance = sLifterParameters.fLifterDistance`.

### 3.7 Position ↔ percent, boot state, limits
- `getLifterPosition()` = raw ticks (1288); `getLifterPositionClamped()` floors at 0 (1302) — used everywhere except inside seek loops.
- `getLifterHeightPercent()` = `clamped * 100 / getLifterDistance()` (4275), falling back to `sLifterParameters.fLifterDistance`.
- `getLifterDistance()` (907) = `sSettings.fLifterDistance` if non-zero else `sLifterParameters.fLifterDistance`.
- **Boot: the encoder is 0 regardless of physical position, and nothing infers position from the limit switches.** `disableMotors()` runs at 6196; the first motion command triggers `ensureSafetyManeuver()` → `seekToTop()` which recovers by driving to the top limit. If the mast is already up, the firmware believes it is at 0 % until then.
- `lifterTopLimit()` (1269) has a **3 ms double-read debounce** for EMI; `lifterBottomLimit()` (1281) is a **single raw read** — asymmetric.
- Polarity via `sSettings.fLifterLimitSetting` (`#PNCL`/`#PNOL`).
- `sSettings.fLifterInverted` (895) flips both the commanded direction (1467) and the ISR's B-pin sense (4658); set from `/api/setinvert`, which also invalidates calibration.

### 3.8 Drift correction — `loop()` 6632–6688
Runs only when `fLifterHolding && fLifterTargetPos > 0 && !fMoveMode && !sCalibrating`, at most every 5 s. `driftPct = sLifterParameters.fDriftCorrectionPct` (0 disables, default 5, web-clamped 0–20). If `|current% - target%| > threshold`, re-`seekToPosition(target, fMinimumPower/100)`. Three consecutive failures → `DRIFT CORRECTION: giving up after 3 failures — clearing hold`.

---

## 4. Motion control — rotary

### 4.1 Position model
- `getRotaryPosition()` = raw ticks, 0 = home (1371).
- `rotaryMotorCurrentPosition()` (2258) = `normalize(ticks * 360 / sRotaryCircleEncoderCount)`; returns **0 when `sRotaryCircleEncoderCount == 0`** — i.e. an unmeasured droid always reports "0°".
- `normalize(deg)` (2151) wraps into [0,360). `shortestDistance(origin,target)` (2160) signed ≤180°. `withinArc(p1,p2,p3)` (2145) fudge-window test.

### 4.2 Four *different* definitions of "home" (a rewrite hazard)
| Function | Line | Semantics |
|---|---|---|
| `rotaryHomeLimit()` | 1309 | Raw single switch read (returns **true** if `fDisableRotary`) |
| `rotaryHomeLimitConfirm(ms=30)` | 1326 | Polls for `ROTARY_HOME_DEBOUNCE_MS` and returns true if the switch closed at any point |
| `rotaryAtHome()` | 1354 | switch **OR** (`sRotaryHomed` **AND** angle within `ROTARY_HOME_TOLERANCE_DEG` = 10°) |
| `isRotaryAtRest()` | 2882 | (switch **OR** `rotaryMotorCurrentPosition() == 0` exactly) **AND** `!fRotaryMoving` |

`seekToPosition` uses `rotaryAtHome()`; `seekToTop`/`seekToBottom` use `isRotaryAtRest()`; the web `/api/lift` uses raw `rotaryHomeLimit()`. Consolidating these is the single highest-value cleanup.

### 4.3 Absolute / relative / continuous
- `rotaryMotorAbsolutePosition(degrees, speed=0, maxspeed=0)` (2265): speed defaults to `ROTARY_MINIMUM_POWER/100`; loops `moveScopeToTarget(current, normalize(degrees), ROTARY_FUDGE_POSITION=5°, speed, maxspeed, m, flipCount)` while checking `sWebAbort` (2280) and `RotaryStatus::isMoving()` (2282); ends with `rotaryMotorStop()`.
- `moveScopeToTarget` (2183): if outside the ±5° arc, scale `speed += (maxspeed-speed)*|dist|/180` clamped to `maxspeed`; **floor at `ROTARY_MINIMUM_POWER/100` (default 40 %)** (2210); direction `nm = (dist>0) ? -speed : speed`. **Direction-flip hysteresis**: two consecutive flips → declare overshoot and stop (2217–2234).
- `rotaryMotorRelativePosition(rel)` (2292) = `rotaryMotorAbsolutePosition(current + rel)` — hence the documented `:PD` defects (§6.1 of the handoff): `normalize()` kills multi-revolution moves and the shortest-path logic caps a single `:PD` at ~180°; speed/maxspeed are dropped.
- `rotaryMotorSpeed(speed)` (2008) = continuous spin target; gated by `rotaryAllowed()`; ramped by `rotaryMotorUpdate()`.
- `rotaryMotorMove(throttle, skipSafety=false)` (2096): `|throttle|` clamped to 1.0, `<0.10` → 0, writes PWM only if `rotaryAllowed() || skipSafety`, else logs `ROTARY NOT ALLOWED: pos=` at most every 2 s.
- `rotaryMotorStop()` (2852) coasts (both PWM 0) but also calls `enableMotors()`. `rotaryMotorBrake()` (2863) shorts the windings (both PWM 1.0).
- `rotaryMotorSpeedForce(speed)` (4324) — rescue-only: zeroes `fRotaryThrottle` first to defeat the change-detection guard, then `rotaryMotorMove(speed, true)`, then syncs `fRotarySpeed`.

### 4.4 `rotaryMotorUpdate()` — 2016–2094, called every `animate()`
1. **Safety cut**: `if (!rotaryAllowed() && fRotaryThrottle != 0)` → log `ROTARY STOPPED: safetyFail=… disabled=… pos=… minH=… home=…`, `rotaryMotorStop()`, `fRotarySpeed = 0` (2026–2040). *This runs on core 1 and will kill a core-0 rescue jog within one loop iteration — the "rotaryMotorUpdate() can override the rescue override" defect.*
2. Every `ROTARY_THROTTLE_LATENCY` (25 ms) ramp `fRotaryThrottle` toward `fRotarySpeed`: accel scale 100, decel scale 20 (used when crossing zero), quantised to 0.01.
3. **Stall check** every `ENCODER_STATUS_RATE*2` = 400 ms: if the tick count is unchanged → `ROTARY NOT MOVING - ABORT` + stop.

### 4.5 Homing
`rotateHome()` (2299):
1. Log `ROTATE HOME: curDeg=… encTicks=… homeSwitch=… circleEnc=…`.
2. **Pass 1** — `rotaryMotorAbsolutePosition(0, 0.5)`, then `rotaryMotorBrake()` + `delay(ROTARY_BRAKE_MS=50)` + stop (added because coasting slid off a just-reached switch). Logs `pass1 done: deg=… home=…`.
3. **Pass 2** — `rotateLeftHome()` or `rotateRightHome()` depending on `shortestDistance(current, 0)`. Logs `pass2 done:`.
4. The old third "precision re-approach" pass is removed (documented at 2340–2345).
5. **Fallback**: if `!rotaryAtHome()` and `circleEnc >= ROTARY_MIN_VALID_CIRCLE_ENC`, print `HOME FAILED (limit switch did not trigger - using encoder position)` and `resetRotaryPosition()` anyway (2357–2361). **This re-zeros the encoder on failure without setting `sRotaryHomed`** — §5.1 of the handoff says this must not happen silently; note it *does* log, but it still moves the reference.

`rotateLeftHome()` (2583) / `rotateRightHome()` (2614): require `rotaryAllowed()`; `found = rotaryHomeLimitConfirm()`, else `rotateUntilHome(∓0.1)`, `delay(200)`, then the opposite direction (right-home retries the reverse **twice**, 2628). On success: `settleOntoHome()` (best effort) → `resetRotaryPosition()` → `sRotaryHomed = true`. `rotateRightHome` logs `FOUND HOME` / `NOT AT HOME`; `rotateLeftHome` logs nothing.

`rotateUntilHome(speed)` (2381) — the v3.6.0 closed-loop creep:
- `speed = ROTARY_MINIMUM_POWER/100 + |speed|` (sign preserved) — so `±0.1` is really ~50 %.
- `ticksPerRev = circleEnc if ≥ ROTARY_MIN_VALID_CIRCLE_ENC (100) else 1000`.
- `targetStep = max(ticksPerRev / ROTARY_CREEP_STEP_DIVISOR(180), 2)` ≈ 2°.
- `searchLimit = ticksPerRev * 5/4` ≈ **1.25 revolutions, distance-based**.
- Loop: drive for `pulseMs` polling the switch continuously (`goto home` on contact); stop; poll again for `ROTARY_CREEP_SETTLE_MS` (12 ms) during coast; then adapt `pulseMs`: moved `< targetStep/2` → grow `×1.5 + 1` capped at `ROTARY_CREEP_PULSE_MAX_MS` (60); moved `> targetStep*2` → shrink `×2/3` floored at `ROTARY_CREEP_PULSE_MIN_MS` (4).
- **Stall guard**: `moved < 2` for `ROTARY_CREEP_STALL_PULSES` (12) consecutive pulses → `CREEP ABORT: rotary will not move (stalled)`.
- Driver fault → `CREEP ABORT: rotary driver fault`.
- Sweep limit → `CREEP: swept a full revolution, no home switch seen`.
- Hard backstop `ROTARY_CREEP_MAX_MS` (25 s); `sWebAbort` checked in both inner polls.
- `home:` label → `rotaryMotorBrake()` + 50 ms + stop; returns whether contact was **seen during the sweep**.

`settleOntoHome()` (2527): returns immediately if `rotaryHomeLimitConfirm()`. Otherwise dithers with growing amplitude — `n = 1..ROTARY_HOME_SETTLE_NUDGES (6)`, alternating direction, target `≈ n` degrees (`ticksPerRev/360` per degree), 1200 ms deadline per nudge, `ROTARY_CREEP_PULSE_MIN_MS` pulses, stopping (not braking) on contact and re-confirming. **Best-effort only — it cannot decide success.**

### 4.6 `#PROTARYTEST` — `rotaryHomeSelfTest(revolutions)` 2662–2850
Preconditions: rotary not disabled; `ensureSafetyManeuver()`; if `!rotaryAllowed()` it will `seekToPosition(1.0, fMinimumPower/100)` to reach safe height, then re-check. `revolutions` clamped 1–10 (default 3 via `#PROTARYTEST`).
Method: spin continuously at `ROTARY_MINIMUM_POWER/100 + 0.10`; deadline `revs*20 s + 10 s`; per sample:
- raw closure counting (`rawClosures` on each open→closed transition);
- **trailing-edge debounce**: `nowHome = everClosed && (millis()-lastClosedMs < ROTARY_HOME_DEBOUNCE_MS)` — this is the fix for the earlier bug that counted chatter as separate revolutions;
- on entering home, record `enterTicks`; on leaving, record the **arc to the last confirmed closure** and the revolution delta `enterTicks - lastEnter` (arrays capped at 12).
Ends with brake + 50 ms + stop.
Report (`---- ROTARY SELF TEST ----`):
- `ABORTED by ESTOP` note if applicable.
- `Home switch NEVER triggered` + advice to check wiring and `#PCONFIG`'s `Rotary Limit` polarity.
- `Home switch detections: N (raw contact closures: M)`; if `M > 2N` → "switch chatters on contact; homing debounces for this".
- `Ticks per revolution: mean/min/max/spread`, `Stored circleEnc:`, and a repeatability verdict at a **5 % spread threshold**.
- `Home contact arc: N ticks (~D deg)`.
- `Creep step: N ticks (~D deg)` plus a warning if `arcMean < creepStep*2`.
**It does not write the measured value back to `circleEnc`** — the handoff doc's §6.2 asks for that.

### 4.7 The web jog path vs. serial
`/api/rotary?s=<-1..1>` (`WebPages.h:2183–2208`): clamps to ±1; blocked unless `lifter.rotaryAllowed()` **or** the rescue override is live; calls `enableMotors()` + `rotaryMotorSpeedForce(speed)` and arms a 10 s jog watchdog. Because it goes through `rotaryMotorMove(..., skipSafety=true)`, the only floor is the generic `<0.10 → 0` in `rotaryMotorMove` — so it can command **0.10** where every serial path floors at `ROTARY_MINIMUM_POWER` (40 %). That is why it was the instrument that proved the switch good. But core 1's `rotaryMotorUpdate()` safety cut (§4.4) will stop it if `rotaryAllowed()` is false, override or not.

### 4.8 Rotary min-power floor sites (all must move together)
`moveScopeToTarget` 2210, `rotaryMotorAbsolutePosition` 2273, `rotateUntilHome` 2388, `settleOntoHome` 2537, `rotaryHomeSelfTest` 2694, `animateAuto` 3883–3886, `animateParameterized` 4085–4088/4106, `:PR` 4962–4965, `:PA/:PD` 5026, `#PS` validator 5710/5757.
`ROTARY_MIN_VALID_CIRCLE_ENC` (100) sites: 2397, 2532, 3224, 3306, 6150, and the `rotateHome` fallback at 2357 — the five the comment at 173–181 warns about, plus one.

### 4.9 `fRotaryMinHeight`
Default `DEFAULT_LIFTER_DISTANCE/2` (231). Gate is `getLifterPositionClamped() > fRotaryMinHeight` inside `rotaryAllowed()` (2003). Set via `/api/params` (`minsafeheight` as a **percentage**, converted at `WebPages.h:2495`) or `setSafeSpinHeightToCurrent()` (4289), which refuses anything below 10 % of travel.

---

## 5. Safety

### 5.1 `safetyManeuver()` — 3186–3365
`SAFETY` → `sSafetyManeuverFailed = false` → `seekToTop(fSafetyManeuverSpeed, usePID=false, updateDistance=false)`.
On success and if the rotary is enabled:
- Light kit → `kLightKit_Dagobah` (visual "I am doing the dangerous thing").
- **Cached path**: `circleEnc >= 100` → `rotateLeftHome()`, else `rotateRightHome()`; if neither → `ABORT: ROTARY HOME NOT FOUND`, `sSafetyManeuverFailed = true`, return false.
- **Measurement path** (up to 5 attempts): nudge with `rotaryMotorMove(-min)`, `rotateLeftHome()`, and if still not home do a full "spin around" until the switch is seen after having left it; then `resetRotaryPosition()`, spin at `min+5` %, count ticks until home returns, `sRotaryCircleEncoderCount = |ticks|`. `< 100` → discard and retry; otherwise persist to `fRotaryEncoderCount` in NVS. No valid count after 5 tries → return false (**without** setting `sSafetyManeuverFailed`).
- Light kit → off; require `isRotaryAtRest()` or `ABORT: ROTARY NOT HOME`.
- `resetLifterPosition()`; `seekToBottom(false)`; `delay(10)`; `sSafetyManeuver = lifterBottomLimit()`. If false → `SAFETY: bottom limit FALSE after seek (switch bounce?) pos=` and `sSafetyManeuverFailed = true`.
On failed `seekToTop` → `ABORT: FAILED SEEK TO TOP`, `sSafetyManeuverFailed = true`.

`ensureSafetyManeuver()` (3368): wizard gate first (`ENSURE SAFETY: blocked — first-run wizard incomplete`), then run the maneuver if `!sSafetyManeuver`; on failure `disableMotors()`.
Callers: `seekToPosition` (1656), `rotaryHomeSelfTest` (2670), `:PH` (5165). Note `:PP`/`:PA`/`:PD`/`:PR` reach it only via `seekToPosition`.

### 5.2 Descend-only-when-homed
Enforcement points, with their differing evaluations:

| Site | Line | Test |
|---|---|---|
| `seekToPosition` clamp | 1661 | `isRotarySpinning() \|\| !rotaryAtHome()` → floor `pos` at `fRotaryMinHeight` fraction |
| `seekToBottom` | 2990 | `!isRotaryAtRest()` → `ABORT: ROTARY NOT HOME` |
| `seekToTop` (PID) | 4414 | `!isRotaryAtRest()` → abort |
| `calibrate` | 3491/3536 | `!isRotaryAtRest()` → retry/abort |
| `/api/lift` | `WebPages.h:2218` | `throttle < 0 && !override && !rotaryHomeLimit() && clamped > fRotaryMinHeight` → refuse |
| `seekToPositionSlow` | 4469 | same as `seekToPosition` (dead code) |

### 5.3 ESTOP
- **Fast path**: `advanceEstopFsm(state, ch)` (1085) — a 4-state scanner for `:` `P` `X` then CR/LF, run from `onUsbSerialReceive` (1103) and `onCmdSerialReceive` (1115) on the **UART driver task**, before the byte hits the FIFO. Sets `sWebAbort = true`. Works on both UARTs.
- **`serialAbort()`** (1634) — the consumer: if `sWebAbort` → log `WEB ABORT`, clear it, clear `sCalibrating`, return true. Also: if `sCalibrating && sUsbFifo.available()` → `SERIAL ABORT`, drain the FIFO, clear `sCalibrating` (any keystroke aborts calibration).
- **Direct `sWebAbort` polls** (which do *not* clear it): `rotaryMotorAbsolutePosition` 2280, `rotateHome` 2323/2334, `rotateUntilHome` 2409/2424/2442, `settleOntoHome` 2538/2544/2564, `rotateLeft/RightHome` 2592–2632, `rotaryHomeSelfTest` 2707/2762.
- **Dispatcher path**: `:PX` handler (4881–4892) clears `sWebAbort`, ends move mode, prints `ESTOP`.
- **Web**: `/api/estop` (`WebPages.h:2247`) sets `sWebAbort`, clears the override, ends move mode, stops both motors, `disableMotors()`, clears jog expiries and `sCalibrating`, `delay(50)`, then clears `sWebAbort`.
- **Not covered by ESTOP**: `seekToPosition`'s pulsed loops check only `serialAbort()`, which is fine; but `breakawayBoost`'s Phase 1/2 also call `serialAbort()` — meaning a `:PX` consumed inside breakaway is *cleared* and the outer seek may not see it. Race is small but real.

### 5.4 Rescue override
`sRescueOverrideExpiry` (1009): set to `millis()+60000` by `/api/action?do=override_on` (`WebPages.h:2293`), cleared by `override_off`, by `/api/estop`, and by the expiry check in `loop()` (6607). Consumed only by `/api/lift` (2219) and `/api/rotary` (2191), and reported by `/api/status` as remaining seconds. **It does not affect any serial path and does not suppress `rotaryMotorUpdate()`'s safety cut.**

### 5.5 Fault state and other overrides
- `/api/action?do=clearfault` — `sSafetyManeuverFailed = false; sSafetyManeuver = true; enableMotors()` (i.e. it asserts the maneuver *passed* without running it). `enablemotors`, `homerotary`, `safety` (which runs `:PH`), `sethome`, `setsafeheight`, `clearenc`, `clearprefs`, `reboot` are the rest.
- `/api/lift` also sets `sSafetyManeuver = true` unconditionally when jogging (`WebPages.h:2233`).
- `animate()` (3836) cancels move mode and stops both motors if `sSafetyManeuverFailed`.
- Wizard gate: `lifterMotorMove` (1453) refuses non-zero throttle unless `wizardComplete() || sWizardMotionOverride`, logging `LIFTER BLOCKED: first-run wizard incomplete` at most every 2 s. **There is no equivalent gate on the rotary.**
- Idle power-down: `isIdle()` (1393) requires the bottom limit **and** `MOTOR_TIMEOUT` (2000 ms) since `enableMotors()`; `loop()` 6822 then calls `disableMotors()` unless holding or jogging.

### 5.6 Known unsafe corners
1. **Both cores drive motors with no lock** (§2.3/2.4). A `:PH` on core 1 and a `/api/wizard/calibrate` on core 0 can interleave `lifterMotorMove()` calls.
2. `rotaryMotorUpdate()` on core 1 nullifies a core-0 rescue jog below safe height.
3. `/api/lift` and `/api/action?do=clearfault` can assert `sSafetyManeuver = true` with no physical validation.
4. `rotateHome()`'s fallback re-zeroes the rotary encoder after a *failed* home (2357–2361).
5. `rotaryAtHome()` is satisfied by a stale `sRotaryHomed` latch plus a ±10° encoder window — the deliberate v3.6.0 trade, but it means a slipped drivetrain still reads "home".
6. `rotaryHomeLimit()` returns **true** when `fDisableRotary` is set (1314) — a "disabled rotary" is unconditionally "home".
7. `breakawayBoost`'s return value is discarded everywhere.
8. `#PR` (toggle rotary disable) reboots immediately (5599–5607) — with the mast wherever it is.
9. `disableMotors()` opens the H-bridge; on a non-self-locking lifter that is a free fall. There is no brake-on-disable.

---

## 6. Persistent state

### 6.1 NVS / `Preferences` — namespace `"uppityspinner"` (`.ino:6107`)
| Key | Macro | Type | Default | Controls |
|---|---|---|---|---|
| `remote` | `PREFERENCE_REMOTE_ENABLED` | bool | `REMOTE_ENABLED` (true) | Droid Remote on/off |
| `rhost` | `PREFERENCE_REMOTE_HOSTNAME` | String | `"R2Uppity"` | SMQ hostname |
| `rsecret` | `PREFERENCE_REMOTE_SECRET` | String | `"Astromech"` | SMQ pairing secret |
| `rpaired` | `PREFERENCE_REMOTE_PAIRED` | bytes | — | Array of `SMQAddressKey`, up to `SMQ_MAX_PAIRED_HOSTS` (20) |
| `rlmk` | `PREFERENCE_REMOTE_LMK` | bytes | — | Local master key (`SMQLMK`) |
| `wifi` | `PREFERENCE_WIFI_ENABLED` | bool | `WIFI_ENABLED` (true) | WiFi on/off |
| `ssid` | `PREFERENCE_WIFI_SSID` | String | `getHostName()` = `R2Uppity-XXXX` | SSID (AP name or STA target) |
| `pass` | `PREFERENCE_WIFI_PASS` | String | `"Astromech"` | WiFi password |
| `ap` | `PREFERENCE_WIFI_AP` | bool | `WIFI_ACCESS_POINT` (true) | AP vs STA |
| `mserial` | `PREFERENCE_MARCSERIAL` | — | — | **Declared at 420 but never read or written** |
| `mwifi` | `PREFERENCE_MARCWIFI_ENABLED` | bool | `MARC_WIFI_ENABLED` (false) | Marcduino over TCP:2000 |
| `lftminpwr` | | int | 65 (`GREG_LIFTER_MINIMUM_POWER`) | `fLifterMinPower` |
| `lftseekbotpwr` | | int | 40 | `fLifterMinSeekBotPower` (also the soft-approach floor) |
| `rotminpwr` | | int | 40 | `fRotaryMinPower` |
| `lftdist` | | int | 450 | `fLifterDistance` (fallback travel) |
| `minheight` | | int | 225 (`dist/2`) | `fRotaryMinHeight` |
| `rotenccount` | | int | 0 | `fRotaryEncoderCount` = cached `circleEnc` |
| `driftpct` | | int | 5 | `fDriftCorrectionPct` (0 = off; web clamps 0–20) |
| `aggression` | | int | 1 (Medium) | `fAggressiveness` |
| `motortype` | | int | 0 (`MOTOR_GREG_6_3_1`) | selects `LifterMotorProfile` |
| `wizstate` | | int | 0 (`WIZARD_NOT_STARTED`) | wizard progress |
| `expertmode` | | bool | false | 19:1 throttle-cap override |

Load/save: `LifterParameters::load()` 722 / `save()` 741 — save writes *all* keys.

### 6.2 EEPROM-backed `LifterSettings` (`EEPROMSettings<LifterSettings> sSettings`, 876–917)
- `fUpLimits[21]` / `fDownLimits[21]` — `{bool valid; unsigned outputLimit;}` per 5 % speed step.
- `fMinimumPower` (default 0) — calibrated lowest workable %.
- `fLifterDistance` (default 0) — measured travel; `getLifterDistance()` falls back to the NVS value.
- Bitfield: `fLifterLimitSetting`, `fRotaryLimitSetting` (NO/NC polarity), `fUpLimitsCalibrated`, `fDownLimitsCalibrated`, `fSafetyManeuver` (**unused**), `fDisableRotary`, `fLifterInverted`.
- `fBaudRate` (default `SERIAL_BAUD_RATE` = 9600), `fID` (default 0), `fReserved1..3` (unused).
- Plus the **stored-sequence area** appended after the struct, magic `0xf005ba11`, `kEndTag = 0xff`, max command length 255, `kMaxCommands = 100`, CRC over the whole EEPROM.

### 6.3 `#PFACTORY` (5253–5261)
`sSettings.clearCommands()` → `preferences.clear()` → `sLifterParameters.load()` (re-reads defaults) → `setMotorProfile()` → print `Cleared` → `reboot()`.
**It does not clear the `LifterSettings` struct itself** — calibration tables, limit polarity, `fLifterInverted`, `fID`, `fBaudRate` and `fDisableRotary` all survive a "factory reset". `#PZERO` (5248) clears only stored sequences. `/api/action?do=clearprefs` clears NVS without rebooting.

---

## 7. Serial pipeline

### 7.1 The two UARTs
| | USB console | Command serial |
|---|---|---|
| Port | `Serial` (UART0) | `Serial2` (`COMMAND_SERIAL`, `.ino:378`) |
| Pins | USB | RX `PIN_RXD2` = **16**, TX `PIN_TXD2` = **0** ("not used" — GPIO0 is a boot-strap pin) |
| Baud | 115200, fixed (`DEBUG_SERIAL_READY()`, line 81) | `sSettings.fBaudRate`, default 9600, `#PBAUD` (>1200), web offers 2400/9600 |
| Framing | — | `SERIAL_8N1` |
| Callback | `onUsbSerialReceive` (1103) | `onCmdSerialReceive` (1115) |
| FIFO | `sUsbFifo` | `sCmdFifo` |
| ESTOP FSM | `sUsbEstopFsm` | `sCmdEstopFsm` |
| Line filter | none | `sCmdSerialAcceptLine` |
| Line buffer | shares `sBuffer` (300) | own `sCmdLineBuf` (256) |

`SerialFifo` (1037–1060): 512-byte lock-free ring, drops on overflow.

### 7.2 Per-line acceptance on the command UART (`loop()` 6718–6752)
A line "belongs to us" only if its **first** byte is `:` or `#` (6740). Everything else — including the SABE / Roam-A-Dome `&<NAME>,HB,...` heartbeats — is consumed and dropped byte-by-byte, terminator included, and `RememberSerialChar()` is not called for it (so the web "last command" widget shows real commands). On CR/LF with `sCmdSerialAcceptLine && sCmdLinePos > 0`, the finished line is `memcpy`'d into `sBuffer`, `sPos` set, and `runSerialCommand()` fired. **The handoff's §5.3 concern stands**: the handoff of a completed command-serial line overwrites `sBuffer`/`sPos`, so a half-typed console line is destroyed.

The USB path (6692–6704) has **no** `:`/`#` filter — every byte lands in `sBuffer`.

### 7.3 Dispatcher (6754–6820)
```
if (sProcessing && millis() > sWaitNextSerialCommand)
    if sCmdBuffer[0]==':'  → chain from sCmdBuffer   (programmatic / stored-sequence path)
    else if sBuffer[0]==':' → chain from sBuffer      (typed / executeCommand path)
    else                    → processCommand(sBuffer, true); resetSerialCommand()
```
Each chain step: find the next `:` from index 1, NUL it, `processCommand(chunk, !sNextCommand)`; on success restore the `:`, `strcpy` the remainder to the front, set `sNextCommand`/`sCmdNextCommand` and print `REMAINS:` under verbose debug; on failure print `Unrecognized:` and drop the rest.

State helpers: `runSerialCommand()` (1130) sets `sProcessing = (sPos != 0)`; `resetSerialCommand()` (1136) sets `sProcessing = (sCmdBuffer[0] == ':')` and zeroes `sPos`; `executeCommand(fmt, …)` (1145) vsnprintf's into `sBuffer`, mirrors into `sCopyBuffer`, and calls `runSerialCommand()`.
`RememberSerialChar()` (6498) maintains `sCopyBuffer` with `\\n`, `\\r`, `\\\\` and `\\xNN` escaping for the web display.

### 7.4 `processCommand(cmd, firstCommand)` — 5900
- **First command**: `:P` → optional decimal ID → if `id != sSettings.fID` silently drop the whole line (`resetSerialCommand(); return true`) → `processLifterCommand(rest)`. `#P` → same ID handling → `processConfigureCommand(rest)`. Anything else → `Invalid`.
- **Chained**: requires `cmd[0]==':' && cmd[1]=='P'` (5907), then `cmd += 2` and the same ID parse (5925–5939). See §7.9 for the consequence.

### 7.5 `:P*` commands — `processLifterCommand` 4824–5204
All of them except `:PL` and `:PW` call `lifter.moveModeEnd()` first.

| Cmd | Grammar | Behaviour |
|---|---|---|
| `:PS<n>` | `n` = `strtolu`, must end the token | Ends move mode; clamps `n` to 0..100; `sSettings.readCommand(n, sCmdBuffer, …, ":P")` — loads the stored sequence **with a single `:P` prefix**; on miss `sCmdBuffer[0]='\0'` |
| `:PP<0-100>[,speed]` / `:PPR[,speed]` | `R` → `random(100)`; speed `strtolu`, clamped 0–100, default 100 %; must end with `\0` or `:` | `Serial.print("POSITION: … SPEED: …")`; `seekToPosition(pos/100, speed)` |
| `:PX` | none | Clears `sWebAbort`, ends move mode, prints `ESTOP`. The real stop already happened in the RX callback |
| `:PM` | bare, or `G`/`M`/`A`, or `[,lift][,rot][,min][,max]` | Bare → `moveModeAutoStart()` with stored aggressiveness; `G/M/A` → per-run override 0/1/2; else parameterized `moveMode()` with defaults `lift = fMinimumPower+5`, `rot = ROTARY_MINIMUM_POWER+5`, `min = 1+random(5)`, `max = min+random(5)`; each parsed arg is floored |
| `:PR<speed>` / `:PRR[max]` | `R` → random in ±speed (default 80) floored at `ROTARY_MINIMUM_POWER`; else signed `strtol` | If `!rotaryAllowed()`, first `seekToPosition(1.0, fMinimumPower/100)`; clamp ±100; `speed == 0` → `rotateHome()`, else `rotaryMotorSpeed(speed/100)` |
| `:PA<deg>[,spd][,maxspd]` | `AR` → `random(360)`; `,R` → `random(100)`; speed floored at `ROTARY_MINIMUM_POWER` | Raise to safe height if needed; `deg == 0` → `rotateHome()`, else `rotaryMotorAbsolutePosition(deg, speed, maxspeed)` |
| `:PD<deg>[,spd][,maxspd]` | identical parse (shares `case 'A'`, `relative = (cmd[-1]=='D')`) | `rotaryMotorRelativePosition(deg)` — **speed and maxspeed are parsed then discarded**, and `normalize()` caps the move at ~180° |
| `:PW[R]<sec>` | `R` → `random(1, sec)` with `sec` defaulting to 6 | `sWaitNextSerialCommand = millis() + max(sec,1)*1000`, **clamped to 60 s** with a `WAIT clamped from Ns to 60s` message |
| `:PL<0-7>` / `:PLR` | `R` → `random(8)` re-rolled while it equals `kLightKit_Off` | `:PL0` first writes Off, `delay(10)`, then 0 (restarts the cycle); `Serial.print("LIGHT: n")`; `setLightShow(n)` |
| `:PH[speed]` | optional trailing digits, clamped `[fMinimumPower, 100]` | Prints a full state line (`:PH cmd — pos=… bot=… top=… home=… rotAllowed=… safety=…`); then, only if `ensureSafetyManeuver() && !lifterBottomLimit() && clamped > 5`: raise to 1.0 if `!rotaryAllowed() && !rotaryAtHome()`, `rotateHome()`, `setLightShow(kLightKit_Off)`, `seekToPosition(0, speed)`; on failure with position > 5, retry the whole raise→home→lower once |
| default | | Ends move mode, prints `Invalid`, returns false (aborts the chain) |

### 7.6 `#P*` commands — `processConfigureCommand` 5211–5896
Evaluated as a chain of `else if`, **in this order** (order matters — `startswith` advances the pointer even when a following `&&` fails):

`SC` (exact) → `RC0|RC1` → `ID<n>` → `ZERO` → `FACTORY` → `RESTART` → `RMASTERKEY0` → `RMASTERKEY` → `DEBUG<0|1>` → `WIFIRESET` → `WIFI[0|1|<empty>]` → `REMOTE<0|1>` → `RNAME<s>` → `RSECRET<s>` → `RPAIR` → `RUNPAIR` → `ROTARYTEST[<1-10>]` → `AGGRESSION[<0-2>]` → `MOTOR[<0-2>]` → `EXPERT[<0|1>]` → `STATUS` → `CONFIG` → `L` → `D<n>` → `BAUD<n>` → `R` → `N{C|O}{L|R}` → `S<n>:<seq>` → else `Invalid`.

Notable semantics:
- `#PSC` — `lifter.calibrate()`; on failure `disableMotors()`.
- `#PRC0/1` — toggles `sRCMode` only (see §14.1).
- `#PID<n>` — sets `sSettings.fID` and writes settings.
- `#PWIFIRESET` — stops the lifter, resets SSID/pass/AP/enabled to defaults, `ESP.restart()`.
- `#PWIFI` with no argument **toggles**; any change reboots.
- `#PRNAME`/`#PRSECRET` reboot on change; `#PRPAIR` starts SMQ pairing; `#PRUNPAIR` removes the paired list and reboots; `#PRMASTERKEY` regenerates and stores the LMK; `#PRMASTERKEY0` deletes it.
- `#PCONFIG` — see §10.4.
- `#PL` / `#PD<n>` — list / delete stored sequences.
- `#PBAUD<n>` — persists only; takes effect at reboot.
- `#PR` — toggles `fDisableRotary`, writes settings, `Serial.flush()`, `delay(1000)`, `ESP.restart()`.
- `#PNCL`/`#PNOL`/`#PNCR`/`#PNOR` — limit switch polarity (lifter / rotary).
- `#PS<n>:<seq>` — validates and stores; see §7.8.
- `UPDATE_SETTING` macro (5206) with `needsUpdate`/`unchanged` exists but **no branch ever sets them** — the trailing `updateSettings()` / `Unchanged` block (5888–5895) is dead.

### 7.7 Other protocols / passthrough
- No JawaLite parser. `WifiMarcduinoReceiver`'s handler merely labels its input `[JAWALITE]` (6309) and feeds it to `executeCommand()`.
- **No serial passthrough to the light kit.** The light kit is driven purely by the three PCF8574 ABC mode bits (`setLightShow`, 2897). README line ~271 documents that builders must set `COMTYPE = 0` on the drolsen light-kit sketch for this to work.
- No `WifiSerialBridge`, no UDP.

### 7.8 `#PS` stored-sequence grammar (5640–5883)
`#PS<n>:<elements>` where `n` is clamped 0..100 and elements are separated by `:`. The validator accepts exactly: `P<pos>[,speed]`, `R<±speed>`, `A<deg>|AR[,speed|,R][,max|,R]`, `D…` (same), `M[,lift][,rot][,min][,max]`, `W[R]<sec>` (clamped 0..600), `L<0-7>|LR`, `H[speed]`. It **echoes** each parsed element to Serial (this is a dry-run validator — it never executes) and, on reaching `\0` cleanly, prints `Sequence [n]: <text>`, clears `sCmdBuffer`, and calls `sSettings.writeCommand(n, startcmd)` → `Stored`/`Failed`. Any unrecognised element → `Invalid` and nothing is stored.
Note there is **no `X` case**, so `#PS9:PX` (README line 220) cannot be stored.

### 7.9 **Defect: multi-element stored sequences are broken (v3.5.2+)**
`git show 3f5913b` changed the chained path from `processLifterCommand(cmd+1)` to `cmd += 2` + ID parse + `processLifterCommand(cmd)`. That fixed chains typed in the *new* form (`:PP100:PL7:PA90`, which the README and web UI use) but broke the *legacy* form, which is exactly what the stored-sequence system produces:

`#PS3:P100:L5:R30` stores `"P100:L5:R30"`; `:PS3` loads it as `":P" + "P100:L5:R30"` = `":PP100:L5:R30"`.
- chunk 1 `":PP100"` → first-command path → `"P100"` → `case 'P'` → seek to 100 % ✔
- chunk 2 `":L5"` → chained path → `cmd[1] == 'L' != 'P'` → **`Invalid`, chain aborted** ✘

And a legacy chunk that *is* `:P<digits>` (e.g. `":P50"` meaning "position 50") is now consumed as an **ID selector** (5926–5939) and silently dropped.

Consequences: every README example sequence except the single-element `#PS1:H`, `#PS2:P100`, `#PS6:A0` executes only its first step. This affects `:PS<n>` from serial, the three external trigger inputs (`loop()` 6869), and the Droid Remote's `SelectScreen` (`menus/SelectScreen.h:53`). The README's own chain example `:PP100:W2:P0` (line 191) is likewise broken. **The store grammar and the replay grammar are now mutually incompatible** — a rewrite must pick one (per-element `:P` prefix is the cleaner choice) and make `#PS` emit it.

---

## 8. Random / move mode

### 8.1 Structure
`animate()` (3831): cancel move mode + stop both motors if `sSafetyManeuverFailed`; `rotaryMotorUpdate()`; if `fMoveMode && fMoveModeNextCmd < millis()` → `animateAuto()` (if `fMoveModeAuto`) or `animateParameterized()`.
Entry points: `moveModeAutoStart(levelOverride = -1)` (4151, level from `fAggressiveness` unless overridden), `moveMode(lift, rot, min, max)` (4124), `/api/movemode`.
Exit: `moveModeEnd()` (4134) — called by **every** `:P*` command except `:PL` and `:PW`, by `/api/movemode?stop=1`, `/api/estop`, `/api/lift`, and by `animate()` on safety failure. It deliberately leaves `fLifterTargetPos` alone so drift correction keeps holding.

### 8.2 `animateAuto()` — 3858–4055 (the 6 cases)
Per-level bands:

| | Lift speed | Rotary speed | Action pool | Wait scale |
|---|---|---|---|---|
| 0 Gentle | 30–50 % | `rotMin+5` … 40 % | {1, 3, 4} only | ×2.0 |
| 1 Medium | 65–85 % | `rotMin+10` … 90 % | all 6 | ×1.0 |
| 2 Aggressive | 80 % … `max(85, effectiveMaxThrottle*100)` | 70–100 % | all 6 | ×0.4 |

Lift speed is floored at `sSettings.fMinimumPower`; rotary at `ROTARY_MINIMUM_POWER + 5`. Final wait is floored at 300 ms (4053).

| # | Name | Sequence |
|---|---|---|
| 0 | **pop up/down** | `rotateHome()` → seek 1.0 → 0.5–2.0 s → seek 0.0 → 0.3–1.0 s → seek 0.7–1.0; next in 2–4 s |
| 1 | **big lift** | `rotateHome()` → pick a target ≥0.30 away from current (10 tries), skip if <0.10 → seek; next in 2–5 s |
| 2 | **spin at top** | seek 1.0 → `rotaryMotorSpeed(±rotSpeed)` for 2–5 s → stop; next in 1–3 s |
| 3 | **random angle** | seek 1.0 → `rotaryMotorAbsolutePosition(30..329, (rotMin+5+random(30))/100)`; next in 3–6 s |
| 4 | **look around** | seek 1.0 → 2–3 × `rotaryMotorAbsolutePosition(45..314, (rotMin+5+random(20))/100)` with 0.8–2.0 s pauses → `rotateHome()`; next in 2–4 s |
| 5 | **bob and weave** | `rotateHome()` → 2–3 × (seek 0.7–1.0, 0.2–0.7 s, seek 0.0–0.19, 0.2–0.7 s) → end at 0.6–1.0; next in 1–3 s |

`serialAbort()` is polled between every sub-step. Every branch that descends below the safe height homes the rotary first — aggressiveness never touches safety.

### 8.3 `animateParameterized()` — 4058–4122
`random(3)`: (0) seek to `random(100)` % at `max(fMoveModeNextLifterSpeed, fMinimumPower)` with a single retry; (1) raise to 1.0 if needed, then a random signed rotary speed in ±`nextRotarySpeed` (default 80), floored at `ROTARY_MINIMUM_POWER+5`, `0` → `rotateHome()`; (2) raise if needed, then `rotaryMotorAbsolutePosition(random(360), speed, maxspeed)` with speed randomised between `ROTARY_MINIMUM_POWER` and `nextRotarySpeed`. Next fire at `random(minInterval, maxInterval)` seconds.
Case 2 will divide by/`random()` a negative range if `fMoveModeNextRotarySpeed < ROTARY_MINIMUM_POWER` (4107) — the `:PM` parser floors it, but `#PS`-stored `M,...` values are not re-floored on replay.

### 8.4 Interleaving
Random mode is cooperative, not pre-emptive: `animateAuto()` blocks inside `seekToPosition`/`rotateHome` for the duration. A queued `:PH` therefore waits (documented in README ~line 208). Only `:PX` / `/api/estop` pre-empt.

---

## 9. Light kit, status LED, display

### 9.1 Light kit
Three PCF8574 pins `PIN_LIGHTKIT_A/B/C` (expander 1/2/3), **active low** — `setLightShow` writes `!((show>>2)&1)`, `!((show>>1)&1)`, `!((show>>0)&1)` (2901–2903). Truth table documented at `.ino:390–408`.

| Value | Name | A B C (as switch positions) |
|---|---|---|
| 0 | `kLightKit_FullCycle` | OPEN OPEN OPEN — random colour/pattern/speed |
| 1 | `kLightKit_Off` | OPEN OPEN GND |
| 2 | `kLightKit_ObiWan` | OPEN GND OPEN |
| 3 | `kLightKit_Yoda` | OPEN GND GND |
| 4 | `kLightKit_Sith` | GND OPEN OPEN |
| 5 | `kLightKit_SearchLight` | GND OPEN GND |
| 6 | `kLightKit_Dagobah` | GND GND OPEN |
| 7 | `kLightKit_Sparkle` | GND GND GND |

`setLightShow(kLightKit_Off)` at the end of `PeriscopeLifter::setup()` (2973). The safety maneuver sets Dagobah while homing (3222) and Off afterwards (3332). `:PH` forces Off before lowering (5178). `getLightShow()` is exposed in `/api/status` as `lightkit`.

### 9.2 Status LED (`PIN_STATUSLED` = 5, one NeoPixel)
`SingleStatusLED<5> statusLED(kStatusColors, 7)`, table at 672–680, all channel values capped at 2 (dim). Mode encoding uses `sCurrentMode` as a 0/1 base (`kNormalModeHome`/`kWifiModeHome`) plus an offset:

| Mode | Value | Colours (4 frames) | Meaning |
|---|---|---|---|
| `kNormalModeHome` | 0 | solid green | rotary at home |
| `kWifiModeHome` | 1 | solid blue | at home, WiFi connected |
| `kNormalModeAway` | 2 | solid red | rotary away from home |
| `kWifiModeAway` | 3 | blue/red alternating | away, WiFi |
| `kNormalModeMoving` | 4 | green/yellow alternating | motors energised, safety OK |
| `kWifiModeMoving` | 5 | blue/yellow alternating | as above, WiFi |
| `kSafetyMode` | 6 | solid yellow | motors energised, safety maneuver not yet passed |

Set at: `enableMotors()` 1431 (`sSafetyManeuver ? sCurrentMode + kNormalModeMoving : kSafetyMode`), WiFi-connected callback 6316 (`sCurrentMode = kWifiModeHome`), and the home-edge detector in `loop()` 6844/6850.

### 9.3 `Screens.h` / `menus/`
`Screens.h` declares 8 IDs. Compiled in **only** when `USE_MENUS`, which `.ino:571–573` defines whenever `USE_DROID_REMOTE` is set — so **yes, it is in the default build**. The "display" is not local hardware: `CommandScreenHandlerSMQ sDisplay` renders over SMQ/ESP-NOW to the handheld Droid Remote's OLED; `sDisplay.process()` runs on core 0 (6426).

Screens present: `MainScreen` (17 items, listed at `menus/MainScreen.h:7–25`, each firing an `executeCommand()`), `RotatePeriscopeScreen` (dial/arrow jog issuing `:PA<deg>,50`, 50 ms key repeat, target 0 remapped to 1 to avoid the slow home path), `SelectScreen` (lists stored sequence numbers via `sSettings.getCommands`, fires `:PS<n>`), `SettingsScreen` (WiFi mode / Calibrate Lifter / Erase All Settings), `WiFiModeScreen` (ON/OFF → `wifi` pref → reboot), `EraseSettingsScreen` (YES/NO → `#PZERO`), `SettingsUpdatedScreen` ("OK" splash for 500 ms).
**`kCalibrateScreen` (Screens.h:9) has no implementation** — `SettingsScreen::kCalibrateLifter` pushes a screen that does not exist (§14.7).

### 9.4 `USE_SDCARD` variant differences
Commented out by default (`.ino:96`). When enabled:
- Limit switches and motor-enable move onto the PCF8574 (`pin-map.h:121–128`); the three external trigger inputs disappear, and `readInputHeaders()` returns 0 (`.ino:6091`), so trigger-driven `:PS<n>` is gone.
- Adds SPI SD pins 4/18/19/23; `PIN_LIFTER_TOPLIMIT`/`BOTLIMIT` are no longer 18/19.
- `setup()` mounts the card, and if `/UPPITY.BIN` exists, flashes it byte-by-byte through `Update`, deletes it, and reboots (6111–6144).
- `mountSDFileSystem` / `unmountSDFileSystem` / `getSDCardMounted` become live; `unmountFileSystems()` is called before web OTA and `reboot()`.
- Build flag route: `make USE_SDCARD=1` (`Makefile:9–11`).

---

## 10. Diagnostics / logging

### 10.1 60 s heartbeat (`loop()` 6550–6600)
```
ALIVE v3.6.0 uptime=<s>s safety=<OK|FAIL|PENDING> height=<pct>% (<ticks>/<dist> ticks)
 lim=[top=0/1 bot=0/1 home=0/1] rot=<deg>deg motors=<ON|off>
 liftFault=<YES|no> rotFault=<YES|no> [HOLD=<pct>%] wifi=<RSSI>dBm|AP(<n>clients)
```

### 10.2 Debug flags
- `USE_DEBUG` (54) — compile-time; enables `DEBUG_*` and the verbose state dump.
- `sVerboseDebug` (1128) — runtime, `#PDEBUG1`/`#PDEBUG0`, not persisted. Gates: rotary throttle prints (2057), `MOVE raw=…` (2188–2244), `ROTARY DEGREE`, `WAIT`, `REMAINS:`, `NO CHANGE`, and the `T: B: R: H: D: P: F:` state dump on change (6920–6949).
- `USE_SMQDEBUG` (87) — commented out.

### 10.3 Web log buffer (`.ino:480–532`)
`WebLogBuffer : public Print`, **50 lines**, each truncated to **120 chars**, ring-buffered with a monotonic index so `/api/log?since=N` returns only new lines as `idx\tmsg\n`. Fed by `_debugPrint*` (546–563) — i.e. by `DEBUG_PRINT`/`DEBUG_PRINTLN` only. **A very large fraction of the firmware's diagnostics use bare `Serial.print` and therefore never reach the web log**: all of `SEEK UP/DOWN`, `BREAKAWAY`, `WIZARD`, `ROTATE HOME`, `SAFETY`, `DRIFT CORRECTION`, `#PCONFIG`, `ROTARY SELF TEST`, and the heartbeat. Anything named `_debugPrint` also suppresses lines starting with `"New client"`.

### 10.4 `#PCONFIG` (5545–5573)
Firmware, Motor Type + profile name, Expert Mode (+ "no-op for active motor"), Wizard State (+ complete/INCOMPLETE), ID#, Baud Rate, Rotary Disabled, Min Power, Distance, Lifter Limit polarity, Rotary Limit polarity, Up Calibrated (**down-calibrated is not printed**), Lifter Min Power `[seek-bottom power]`, Rotary Min Power, Lifter Distance, Rotary Min Height, Safety Maneuver, `Safety Maneuver FAILED` if applicable (missing a newline), then `Stored Sequences:` + `sSettings.listSortedCommands(Serial)`.

### 10.5 `#PSTATUS` (5522) — just `WiFi Enabled/Disabled` and `Remote Enabled/Disabled`.

### 10.6 Boot banner
`PrintReelTwoInfo(Serial, "Uppity Spinner")` (6160) → ReelTwo build info; then `Settings Restored`/`First Time Settings`, the wizard note, `Motor profile: <name>` (from `setMotorProfile`), `R2UppitySpinner v3.6.0`, `READY`.

### 10.7 Logging hazards the handoff calls out, still present
- `loop()`'s `HOME` print is **both** edge-triggered (`sRotaryHomeStatus != rotaryIsHome`) **and** 2 s-throttled (6837–6841), and sampled once per iteration — the double aliasing that misled the September investigation.
- `HOME FAILED (limit switch did not trigger - using encoder position)` (2359) is a failure that then re-zeroes the reference.

---

## 11. `LifterMotorProfile`

Struct: `.ino:256–291`. Selection: `sLifterParameters.fMotorType` → `setMotorProfile()` (761) → `sMotor`. Default `&kProfileGreg6p3` (371).

| Field | Greg 6.3:1 (`kProfileGreg6p3`, 294) | Pololu 19:1 (`kProfilePololu19`, 327) | IA-Parts (`kProfileIAParts`, 348) |
|---|---|---|---|
| `fName` | `"Pololu 4757 (6.3:1, original)"` | `"Pololu 4751 (19:1, high-torque)"` | `"IA-Parts"` |
| `fDefaultMinPower` | 65 | 45 | 30 |
| `fDefaultSeekBotPower` | 40 | 30 | 30 |
| `fDefaultLifterDistance` | 450 | 1350 | 845 |
| `fMaxCommandedThrottle` | 1.0 | **0.75** | 1.0 |
| `fCalibrationMaxSweepPct` | 100 | **85** | 100 |
| `fStallTimeoutMs` | 2000 | **700** | 2000 |
| `fSlowProgressMinTicksPerSec` | 10 | **25** | 10 |
| `fSlowProgressMaxMs` | 3000 | **1500** | 3000 |
| `fPulseOnMs` | 3 | **5** | 3 |
| `fPulseOffMs` | 1 | **2** | 1 |
| `fBreakawayMaxMs` | 500 | **250** | 500 |
| `fBreakawayRampTicksCap` | 80 | **40** | 80 |
| `fCalibrationCooldownMs` | 2500 | **4000** | 2500 |
| `fSafetyManeuverSpeed` | 0.8 | **0.6** | 0.8 |
| `fSoftApproachEnabled` | false | **true** | false |
| `fSoftApproachPct` | 0 | **15** | 0 |

Rationale block at 314–326 (Greg Hulette cracked his lifter frame calibrating the 19:1).
`applyMotorProfileDefaults()` (778) copies only min/seek-bottom power and distance into `sLifterParameters` — rotary values are untouched.
Changing motor type via `wizardChangeMotorType()` (925) or `changeMotorTypeBypassWizard()` (954) invalidates both calibration tables, zeroes `fLifterDistance` and `fMinimumPower`, and writes settings; the first resets the wizard, the second marks it complete.

**Expert Mode** (`fExpertMode`, `#PEXPERT`, Parameters page): only meaningful when `fMotorType == MOTOR_POLOLU_19_1` (`expertModeApplies()`, 809). Effects: `effectiveMaxThrottle()` 0.75 → 1.0 (793) and `effectiveCalibrationSweepCap()` 85 → 100 (800). Deliberately **not** raised: the wizard creep speed and `fSafetyManeuverSpeed` (comment at 786–792). `#PEXPERT1` warns about frame damage and tells you to re-calibrate.

---

## 12. Networking

- **WiFi** (`WIFI_ENABLED`, `.ino:120–124): default **AP mode**, SSID from `getHostName()` = `"R2Uppity-" + last-4-of-MAC` (6025–6032, note the compiled-in `WIFI_AP_NAME` is `"R2Uppity"` and the `wifi` web form's fallback default is `WIFI_AP_NAME` while `setup()`'s is `getHostName()` — a small inconsistency), password `"Astromech"`, AP IP `192.168.4.1`. STA mode retries every 30 s (WifiAccess default interval). `wifiAccess.animate()` runs on core 1.
- **Web server**: `WifiWebServer<20, N>` on port 80, `handle()` on core 0. Pages: `/` (302 → `/periscope`), `/periscope`, `/rescue`, `/log`, `/setup`, `/calibrate`, `/wizard`, `/parameters`, plus `WElement` forms `/marcduino`, `/wifi`, `/remote`, `/firmware`. JSON APIs: `/api/cmd`, `/api/rotary`, `/api/lift`, `/api/estop`, `/api/action`, `/api/status`, `/api/log`, `/api/wizard/{status,setmotor,advance,creep,calibrate,accept,reset,seek,skip}`, `/api/setmotor`, `/api/setinvert`, `/api/params`, `/api/params/save`, `/api/params/safespin_current`, `/api/params/defaults`, `/api/movemode`. Upload: `/upload/firmware`.
- **mDNS** (`USE_MDNS`): `MDNS.begin(getHostName())` only when **not** SoftAP (6321–6331). With the default AP config, mDNS never starts.
- **OTA**: three mechanisms.
  1. **Web upload** (`/upload/firmware` + `/firmware` page) — `Update.begin/write/end`, unmounts filesystems, sets `sOTAInProgress`, restarts on success. **This is the one that works.**
  2. **`ArduinoOTA`** — callbacks registered (6337–6368) and `ArduinoOTA.handle()` polled (6407), but **`ArduinoOTA.begin()` is never called anywhere**. Arduino-IDE network OTA is dead.
  3. **SD card** — `/UPPITY.BIN` at boot, `USE_SDCARD` only.
- **Marcduino over WiFi** (`USE_WIFI_MARCDUINO`): TCP port 2000, off by default (`MARC_WIFI_ENABLED false`), enabled by the `mwifi` pref / `/marcduino` page.
- **SMQ / Droid Remote** (`USE_DROID_REMOTE`, default on): ESP-NOW via `ReelTwoSMQ32.h`. `WiFi.mode(WIFI_MODE_APSTA)` (6204), `SMQ::init(hostname, secret)`, LMK restore, paired-host restore (up to 20), pairing / discovery / lost callbacks. Messages handled: `DIAL`, `BUTTON`, `SELECT` (6436–6458). `DisconnectRemote()` (6462) sends an `EXIT` topic on reboot. Fixed `WIFI_CHANNEL 1`, 1 s beacon, 10 s host-lost timeout.
- **No UDP, no serial-over-TCP.**

---

## 13. Build

- **Board**: `esp32dev` / `esp32:esp32:esp32`, ESP32-WROOM-32. `platformio.ini` pins platform `espressif32 v5.2.0`, `f_flash = 80 MHz`, `flash_mode = qio`, `monitor_speed = 115200`, `build_type = release`, flags `-DCORE_DEBUG_LEVEL=3 -mfix-esp32-psram-cache-issue -ffunction-sections -fdata-sections -Wl,--gc-sections -Os`. `lib_deps`: reeltwo/Reeltwo, adafruit/Adafruit_NeoPixel, reeltwo/PCF8574. `src_dir = .`, `build_src_filter = +<*>`.
- **Makefile**: reeltwo `Arduino.mk` (`include ../Arduino.mk`), `TARGET=ESP32`, `PORT=/dev/ttyUSB0`, `ESP32_PSRAM=disabled`, `ESP32_DEBUGLEVEL=verbose`, and `USE_SDCARD=1` → `-DUSE_SDCARD=1`.
- **No partition scheme is specified** in either build file — it uses the board default. Given the recorded **~94 % of program storage (~1.24 MB of 1.31 MB)** in `CLAUDE.md` / `docs/REWRITE-HANDOFF.md §7`, this is the default 1.3 MB app partition (`default.csv`), which is why OTA works but headroom is the binding constraint on the rewrite.
- **`build_version.h` / `reeltwo_build_version.h`** are optional (`__has_include`, 37–45); `BUILD_VERSION` defaults to `"custom"` and surfaces as a "Sources" link on the Firmware page.
- **`USE_*` and other top-level defines** (`.ino:54–96`, 380–386):

| Define | Default | Enables |
|---|---|---|
| `USE_DEBUG` | on | `DEBUG_*` output and the verbose state dump |
| `USE_SMQDEBUG` | **off** | very verbose ESP-NOW tracing |
| `USE_DROID_REMOTE` | on | SMQ remote; also implies `USE_MENUS` (571–573) and pulls `ReelTwoSMQ32.h` instead of `ReelTwo.h` |
| `USE_WIFI` | on | WifiAccess + the core-0 event task |
| `USE_WIFI_WEB` | on | `WebPages.h`, `WebLogBuffer`, jog watchdogs, rescue override |
| `USE_WIFI_MARCDUINO` | on (feature itself off by pref) | TCP:2000 receiver |
| `USE_MDNS` | on | `.local` hostname (STA only) |
| `USE_OTA` | on | ArduinoOTA callbacks (service never started) |
| `USE_SDCARD` | **off** | SD pin map + boot reflash from `/UPPITY.BIN` |
| `USE_I2C_GPIO_EXPANDER` | on (in `pin-map.h`, both variants) | PCF8574 routing for pins ≥ `GPIO_PIN_BASE` (200) |
| `DISABLE_ROTARY` | **off** | compiles the rotary out and implies `DISABLE_SAFETY_MANEUVER` |
| `INTERNAL_LIFTER_DEBUG` | **off** (`#undef` at 3102) | bench motor-test routines |
| `USE_SPIFFS` / `USE_FATFS` / `EEPROM_FLASH_PARTITION_NAME` | never defined | leave `mountReadOnly/WritableFileSystem` dead and EEPROM without a dedicated partition |

---

## 14. Everything else, plus defects

### 14.1 RC / PPM support is dead
`sPPM.begin()` is called only at `.ino:6392`, guarded by `if (sRCMode)`. `sRCMode` is a plain static defaulting to false and is **never loaded from any preference** — `#PRC1` (5229) just sets the flag at runtime. So the PPM decoder is never started, and the RC block in `loop()` (6877–6903) reads a decoder that was never initialised. `sPPM.end()` in `reboot()` (1238) is likewise unreachable in practice.

### 14.2 External trigger inputs (standard board only)
`PIN_INPUT_A/B/C` = expander bits 0, 6, 7 with `INPUT_PULLUP` (2966–2968). `readInputHeaders()` (6080) returns `(!A)<<2 | (!B)<<1 | (!C)<<0`, so grounding A/B/C gives 4/2/1 and combinations give 1–7. On any PCF8574 interrupt-driven change, `loop()` (6854–6876) fires `:PS<value>` for non-zero values — i.e. up to 7 stored sequences on hardware jumpers. Broken past their first element (§7.9).

### 14.3 EEPROM persistence is suspect on ESP32 — **verify on hardware**
`EEPROMSettings.h` only calls `EEPROM.commit()` inside `#ifdef EEPROM_FLASH_PARTITION_NAME`, which is **never defined** here, so `sSettings.write()` writes the RAM mirror and never flushes to flash. Separately, `EEPROM_SIZE` expands to `EEPROM.length()`, and `setup()` calls `EEPROM.begin(EEPROM.length())` **before** any `begin()` — on the ESP32 core `length()` returns 0 at that point and `begin(0)` returns false, in which case the `else` at 6168 means `sSettings.read()` is never reached and the log prints `Failed to initialize EEPROM`. If that is what actually happens, calibration tables, limit polarity, `fID`, `fBaudRate`, `fDisableRotary`, `fLifterInverted` and all stored sequences are **volatile across reboots**, and the surviving persistence is entirely NVS/`Preferences`. This is worth confirming against a real boot log before designing the rewrite's storage layer.

### 14.4 `#endif` / brace mismatch breaks non-mDNS builds
`.ino:6318–6333`: the closing `}` of `if (webServer.enabled())` sits **inside** the `#ifdef USE_MDNS` block, with the `#endif` after it. Turning off `USE_MDNS` will not compile.

### 14.5 Dead `STATUSLED_PIN` block
`.ino:6475–6477` is guarded by `#ifdef STATUSLED_PIN` (the real macro is `PIN_STATUSLED`) and references `kNormalMode`, which does not exist. Unreachable, would not compile if reached.

### 14.6 `#PMINPWR` does not exist
`calibrate()` (3824) tells the user to raise min power "via Parameters page or `#PMINPWR` command". There is no such command — the only routes are `/api/params/save` and the wizard.

### 14.7 `kCalibrateScreen` has no screen class
`Screens.h:9` declares it and `menus/SettingsScreen.h:39` pushes it, but no `menus/CalibrateScreen.h` exists. "Calibrate Lifter" on the Droid Remote pushes a non-existent screen.

### 14.8 `startswith()` mutates on partial match
`startswith(cmd, str)` (4811) advances `cmd` whenever the prefix matches, including in `startswith(cmd,"X") && isdigit(*cmd)` expressions where the second test then fails. So `#PID`, `#PDEBUG`, `#PREMOTE` (no argument) leave `cmd` advanced and fall through the remaining `else if` chain with a mangled pointer. Currently they all end at `Invalid`, but it is fragile.

### 14.9 Other dead / vestigial code
- Never called: `isCalibrated()` (2983), `seekToPositionSlow()` (4464), `lifterMotorBrake()` (1506), `WebLogBuffer::getLatestIdx()` (522), `mountReadOnlyFileSystem` / `mountWritableFileSystem` (1167/1175).
- Never used: `otaInProgress` (4761), `LifterSettings::fSafetyManeuver` and `fReserved1..3`, `PREFERENCE_MARCSERIAL` (420), the `UPDATE_SETTING` macro + `needsUpdate`/`unchanged` block (5206, 5888–5895), local `safeMin` in `animateAuto` (3862).
- `encoder_rotary_stop_limit` / `encoder_rotary_stop_ticks` (4612–4616) are set and cleared but the ISR consumer is commented out (4679–4689).
- `INTERNAL_LIFTER_DEBUG` bench routines (3104–3183, 3188–3211) are `#undef`'d out.
- The `TargetSteering` in `seekToPosition` is constructed, tuned, and then bypassed (`limit = 0`, 1708/1851).
- `seekToBottom(usePID = true)` and `seekToTop(usePID = true)` PID branches are effectively unreachable from any command path.

### 14.10 Miscellaneous behaviours worth preserving or deciding on
- `MOTOR_TIMEOUT` power-down only happens **at the bottom limit** (`isIdle()`, 1393) — a mast parked at 60 % keeps the drivers energised indefinitely.
- `rotaryMotorStop()` calls `enableMotors()` — a "stop" that powers the bridge on.
- `lifterMotorMove` / `rotaryMotorMove` write PWM only on a *change* of throttle value; `rotaryMotorSpeedForce` exists purely to defeat that guard (4318–4323).
- `:PW` is capped at 60 s in the live parser but at 600 s in the `#PS` validator (5808).
- `getLifterDistance()` and `getLifterHeightPercent()` disagree about which distance to trust in different call sites; height % is computed from `sSettings.fLifterDistance` (calibrated) while `animateAuto`'s `safeMin` uses the same but `setSafeSpinHeightToCurrent` uses `sLifterParameters.fLifterDistance`.
- The web `/api/params/defaults` only offers Greg 6.3:1 and IA-Parts presets — there is no 19:1 preset there, though `#PMOTOR1` / the wizard apply the profile defaults.
- `no TODO/FIXME/XXX/HACK comments exist anywhere in the sketch, headers or menus` — the known-issue list is entirely in `docs/REWRITE-HANDOFF.md §6` and the changelog.
