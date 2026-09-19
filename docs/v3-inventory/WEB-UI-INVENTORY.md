# v3.6.0 web UI inventory and critique

Generated 2026-09-07 by an exploration pass over `WebPages.h`, `web-images.h` and the web-related parts of the sketch at v3.6.0 (commit 18ce9f3). Candid by design: it is the brief for the v4 redesign.


## Files

| Short name | Absolute path |
|---|---|
| `WebPages.h` | `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/.claude/worktrees/code-session-best-practices-eaf5b8/WebPages.h` (2714 lines, 145 KB) |
| `web-images.h` | `…/code-session-best-practices-eaf5b8/web-images.h` (44 lines, 2.2 KB) |
| `.ino` | `…/code-session-best-practices-eaf5b8/R2UppitySpinnerV3.ino` (6952 lines) |
| ReelTwo web lib | `…/code-session-best-practices-eaf5b8/libraries/Reeltwo/src/wifi/WifiWebServer.h` |

**Version drift, up front:** `.ino:103` is `FIRMWARE_VERSION "3.6.0"`, but `WebPages.h` has not been touched since the v3.5.2 era (`git log -- WebPages.h` → last commit `4baabdf`, "Add GitHub releases link to Firmware page"). Everything the v3.6.0 rotary rebuild added — `sRotaryHomed`, `rotaryAtHome()`, `#PROTARYTEST` — is **invisible to the web UI**. Zero hits for those symbols in `WebPages.h`.

---

# 1. Routes / pages

Route table: `WebPages.h:2085-2711` (`WPage pages[]`, 36 entries). Dispatch is exact-string match on the path with the query stripped (`WifiWebServer.h:1465-1487`, `getPage()`); everything is **GET** except one POST upload. No auth, no CSRF, no sessions.

### HTML pages

| URL | `<title>` | Renderer | Purpose |
|---|---|---|---|
| `/` | — | `WebPages.h:2086` | 302 → `/periscope` |
| `/periscope` | "R2 Periscope v3.6.0" | `printPeriscopePage` `WebPages.h:474` | Main operator page |
| `/rescue` | "R2 Rescue" | `printRescuePage` `WebPages.h:813` | Manual recovery / jog |
| `/log` | "R2 Log" | `printLogPage` `WebPages.h:1039` | Live serial log tail |
| `/setup` | "R2 Setup" | `printSetupPage` `WebPages.h:1096` | Settings hub (list of rows) |
| `/calibrate` | "R2 Calibrate" | `printCalibratePage` `WebPages.h:1248` | Motor type / invert / calibration |
| `/parameters` | "R2 Parameters" | `printParametersPage` `WebPages.h:1365` | Tuning knobs |
| `/wizard` | "R2 Setup Wizard" | `printWizardPage` `WebPages.h:1665` | First-run bring-up |
| `/marcduino` | (ReelTwo shell) | `marcduinoContents` `WebPages.h:1971` | ID / baud / WiFi-Marcduino |
| `/wifi` | (ReelTwo shell) | `wifiContents` `WebPages.h:2003` | SSID / pass / AP mode |
| `/remote` | (ReelTwo shell) | `remoteContents` `WebPages.h:2033` | SMQ droid remote |
| `/firmware` | (ReelTwo shell) | `firmwareContents` `WebPages.h:2058` | OTA upload |

### Navigation

Single source of truth: `struct NavTab kNavTabs[]` at `WebPages.h:298-304` — **four** tabs only: `periscope`, `rescue`, `log`, `setup`.

- **Phone/tablet (<1024 px):** fixed bottom tab bar, `#tabs`, emitted by `printTabBar()` `WebPages.h:340-358`. CSS `WebPages.h:192-209`.
- **Desktop (≥1024 px):** left sidebar `#sidenav`, emitted **server-side** inside `printStatusBar()` `WebPages.h:327-337`. CSS `WebPages.h:240-257`. `#tabs{display:none}` at `WebPages.h:259`, `#sidenav{display:none}` at `WebPages.h:211`.
- Both bars are emitted on *every* page and CSS picks one. The comment at `WebPages.h:307-308` records that the sidebar used to be built by inline JS and "sometimes failed silently on desktop browsers, leaving no nav at all" — that specific race is fixed.

**Reachability graph:**
- `/periscope`, `/rescue`, `/log`, `/setup` — from any page (tab bar / sidebar).
- `/wizard`, `/calibrate`, `/marcduino`, `/wifi`, `/remote`, `/parameters`, `/firmware` — **only** from `/setup` rows (`WebPages.h:1109, 1132, 1144, 1158, 1178, 1196, 1212`), or by typing the URL.
- Those seven pages have **no way back except the tab bar**, which returns you to a top-level tab, not to `/setup`. `/parameters` has an explicit "Back" link (`WebPages.h:1525`); `/calibrate` and `/wizard` do not. The four ReelTwo pages have a "Back" `<input type=button>` (`WebPages.h:1995, 2029, 2054, 2077`).
- `/wizard` renders **no nav at all** — no tab bar, no sidebar, no status bar. Its only exits are "Go to main control" (`WebPages.h:1942`) and the browser back button.
- The `/parameters` expert card links to `/calibrate` (`WebPages.h:1500`).

---

# 2. API endpoints

All are `WAPI` lambdas in `WebPages.h`. All parse the query string with `qs.indexOf("key=")` + `substring` — no real parser, no URL-decoding except in `/api/cmd`. All hand-roll JSON with `out.print()` — **no ArduinoJson anywhere** in the project.

### `/api/status` — `WebPages.h:2308-2348`
No params. Fields, in emission order:

`version` (string), `height` (int %, `getLifterHeightPercent()`), `ticks` (long), `rotPos` (long, raw rotary encoder), `rotDeg` (unsigned 0-359), `safety` (bool `sSafetyManeuver`), `fault` (bool `sSafetyManeuverFailed`), `motors` (bool), `topLimit`, `botLimit`, `rotHome`, `liftFault`, `rotFault` (bools), `lightkit` (int 0-7), `holding` (bool), `rssi` (int, 0 if not STA), `apClients` (int), `override` (int seconds remaining, 0 = off; snapshotted at `WebPages.h:2334` to avoid a torn cross-core read), `lastCmd` (string from `sCopyBuffer`, only `"` escaped — a `\` or control char in the buffer produces invalid JSON).

Called by every page. **Polling intervals:** periscope 1000 ms (`:805`), rescue **500 ms** (`:1015`), log 2000 ms (`:1070`), setup 1000 ms (`:378`), calibrate 1000 ms (`:1353`), parameters 1000 ms (`:1653`).

### Motion
| URL | Params | Response | Side effects | Callers |
|---|---|---|---|---|
| `/api/cmd` `:2155` | `c=` (URL-decoded, hand-rolled decoder `:2161-2173`) | `{"ok":true}` | `executeCommand()` — **any** `:P*` or `#P*` command, including `#PFACTORY`, `#PRESTART` | periscope (up/down/spin/`:PA`/`:PL`/`:PS`), calibrate (`#PSC`) |
| `/api/lift` `:2210` | `t=` −1..1 | `{"ok":true}` / `{"ok":false,"blocked":"lift"}` | Clears `fLifterTargetPos`/`fLifterHolding`; **sets `sSafetyManeuver = true`** (`:2235`); `moveModeEnd()`; `enableMotors()`; 10 s jog expiry | periscope canvas, rescue jog |
| `/api/rotary` `:2183` | `s=` −1..1 | `{"ok":true}` / `{"ok":false,"blocked":"rotary"}` | `rotaryMotorSpeedForce()` — **bypasses the 40 % `ROTARY_MINIMUM_POWER` floor** (floors at 0.10 in `rotaryMotorMove`); 10 s expiry | rescue shuttle dial, re-sent every 500 ms `:974` |
| `/api/estop` `:2247` | none | `{"ok":true}` | `sWebAbort=true`; clears `sRescueOverrideExpiry`; `moveModeEnd`; both motors stop + disable; clears jog expiries + `sCalibrating`; `delay(50)`; `sWebAbort=false` | periscope, rescue, wizard |
| `/api/movemode` `:2521` | `stop=1` or `level=g\|m\|a` | `{"ok":true,"running":…,"level":N}` | `moveModeEnd()` then `moveModeAutoStart(level)` | periscope random row |

### `/api/action?do=…` — `WebPages.h:2268-2305`
Nine verbs. Anything unrecognised still returns `{"ok":true}` — silent no-op:

| `do=` | Effect | Line |
|---|---|---|
| `clearfault` | `sSafetyManeuverFailed=false; sSafetyManeuver=true; enableMotors()` | `:2274` |
| `enablemotors` | same minus the log line | `:2278` |
| `homerotary` | clears fault, `sSafetyManeuver=true`, `rotateHome()` | `:2281` |
| `safety` | clears fault, `sSafetyManeuver=false`, `executeCommand(":PH")` | `:2285` |
| `sethome` | `setRotaryHome()` | `:2289` |
| `setsafeheight` | `setSafeSpinHeightToCurrent()` — **does not persist** (no `.save()`) | `:2291` |
| `clearenc` | `clearRotaryEncoderCount()` | `:2292` |
| `override_on` | `sRescueOverrideExpiry = millis()+60000` | `:2293` |
| `override_off` | `sRescueOverrideExpiry = 0` | `:2295` |
| `clearprefs` | `preferences.clear()` | `:2297` |
| `reboot` | `reboot()` | `:2299` |

### Config / params
| URL | Params | Response | Notes |
|---|---|---|---|
| `/api/params` `:2436` | — | `lftminpwr, lftseekbotpwr, rotminpwr, lftdist, minsafeheight, driftpct, rotdisabled, aggression, expert, expertapplies, motortype` | See §6 for two denominator bugs |
| `/api/params/save` `:2460` | all of the above as query args, each optional | `{"ok":true}` | Writes live globals **and** `sLifterParameters.save()` |
| `/api/params/defaults` `:2555` | `type=0\|2` | `{"ok":true}` | **Mutates live running state without saving.** No `type=1` (19:1) branch |
| `/api/params/safespin_current` `:2506` | — | `{"ok":true,"pct":N}` | Persists (unlike `action?do=setsafeheight`) |
| `/api/setmotor` `:2401` | `type=0\|1\|2` | `{"ok":true,"reboot":true}` | `changeMotorTypeBypassWizard()` — sets `WIZARD_COMPLETE`, wipes calibration |
| `/api/setinvert` `:2417` | `value=0\|1` | `{"ok":true,"inverted":bool}` | Wipes calibration, `sSettings.write()` |
| `/api/log` `:2350` | `since=N` | `text/plain`, lines `"idx\tmsg\n"` | 50-line ring buffer, `WebLogBuffer` `.ino:480-531`, 120 chars/line max |

### Wizard
| URL | Params | Response | Line |
|---|---|---|---|
| `/api/wizard/status` | — | `state, motor, motorName, maxThrottle, stallTimeoutMs, topLimit, botLimit, lifterPos, rotHome, calibrated, minPower, inverted` | `:2362` |
| `/api/wizard/setmotor` | `type=` | `{"ok":true,"reboot":true}` | `:2384` |
| `/api/wizard/advance` | `to=N` | `{"ok":true}` / `{"ok":false,"err":"bad_state"}` | `:2581` |
| `/api/wizard/creep` | `dir=up\|down` | `{"ok":bool,"result":1\|0\|-1}` | `:2595` |
| `/api/wizard/calibrate` | — | `{"ok":bool}` | `:2615` |
| `/api/wizard/seek` | `pct=0..100` | `{"ok":bool}` | `:2652` |
| `/api/wizard/accept` | — | `{"ok":true}` | `:2629` |
| `/api/wizard/reset` | — | `{"ok":true}` | `:2640` |
| `/api/wizard/skip` | — | `{"ok":true}` | `:2672` |

`/api/wizard/status` is polled at **600 ms** by `/wizard` (`:1949`) and called **once** by `/periscope` at load (`:599`) purely to clamp the lift-speed slider to `maxThrottle`, and by `/calibrate` (`:1301`) for motor name + invert state.

### Upload
`WUpload("/upload/firmware")` `:2683-2710` — POST multipart. `Update.begin/write/end`, `unmountFileSystems()`, `ESP.restart()` on success. Emits the malformed status line `HTTP/1.0 200 FAIL` on error (`:2685`).

---

# 3. First-run wizard (`/wizard`)

### State machine
`enum WizardState` `.ino:697-706`: `NOT_STARTED=0, MOTOR_SELECT=1, SENSOR_CHECK=2, CREEP_TEST=3, FIND_MIN_POWER=4, CALIBRATION=5, ACCEPTANCE=6, COMPLETE=7`.

**Storage:** `sLifterParameters.fWizardState` (`.ino:719`), persisted to NVS key `"wizstate"` (`.ino:433`) via `LifterParameters::save()` (`.ino:752`). Loaded at `.ino:737`.

### The gate
`wizardAllowsMotion()` `.ino:833-836` = `wizardComplete() || sWizardMotionOverride`. Only **two** call sites:
- `lifterMotorMove()` `.ino:1453` — non-zero lifter throttle is dropped, with a 2 s-throttled `"LIFTER BLOCKED: first-run wizard incomplete"` log.
- `ensureSafetyManeuver()` `.ino:3372`.

The **rotary motor is not gated at all**. `rotaryMotorMove()`/`rotaryMotorSpeedForce()` never consult the wizard. On a fresh un-set-up board the rescue shuttle dial will spin the head.

### Steps as rendered (`printWizardPage` `WebPages.h:1665-1951`)
Client-side `render()` dispatch at `:1783-1789`.

**Step 1 — Motor Select** (`renderMotorSelect` `:1792`, states 0-1). Dropdown of the three motors. Five mandatory checkboxes (`c1`-`c5`, `:1802-1807`): frame condition / limit switches wired / encoder wired / travel area clear / risks understood. "Apply Motor & Continue" stays `disabled` until all five are ticked (`:1812-1815`). Second button: **"Expert mode: skip wizard"** → `confirm()` → `/api/wizard/skip` → jumps straight to `COMPLETE` (`:1824`, `:2672`). `setMotor()` (`:1818`) calls `/api/wizard/setmotor`, which wipes calibration and forces `WIZARD_SENSOR_CHECK`.

**Step 2 — Manual Sensor Check** (`renderSensorCheck` `:1828`, state 2). Instructional only; asks the builder to move the carriage by hand and watch the live sensor strip. Explicitly says rotary home is *not* hand-verifiable "the gearing prevents hand movement" (`:1838`). One ack checkbox gates Continue. Back → `advance(1)`.

**Step 3 — Low-Power Creep Test** (`renderCreepTest` `:1850`, state 3). Two buttons: Creep Down, Creep Up. Continue is enabled only once **both** have returned `ok` in this browser session — tracked in `window._creep_up` / `window._creep_down` / `window._creepOk` (`:1867`, `:1876-1877`). Purely client-side: **a page reload loses it** and the user must re-creep.

`wizardCreepToLimit(bool goDown)` — `.ino:3404-3466`:
- speed = `max(0.30 × fMaxCommandedThrottle, fDefaultMinPower/100)`, clamped to the ceiling. For 19:1: `max(0.225, 0.45)` = **0.45**, not 30 %. The card's copy says "runs the motor at 30% throttle" (`WebPages.h:1853`) — wrong for two of the three motors.
- **Continuous drive, not pulsed** (`.ino:3425-3427` comment: pulsing "only chokes off the motor").
- Stall window `CREEP_STALL_MS = 1500`, absolute `CREEP_MAX_MS = 30000`.
- Re-asserts throttle every loop, `delay(5)`.
- Returns `1` limit reached, `0` stall/timeout, `-1` abort.
- Caller wraps it in `sWizardMotionOverride = true/false` (`WebPages.h:2602-2604`).

**Step 4/5 — Calibration** (`renderCalibration` `:1882`, states 4 **and** 5 share one screen). Single "Run Full Calibration" button → `/api/wizard/calibrate` → `lifter.calibrate()`. On success, `wizardAdvance(WIZARD_ACCEPTANCE)` (`:2620`) — note this skips state 5 entirely; `WIZARD_FIND_MIN_POWER` and `WIZARD_CALIBRATION` are never distinguished at runtime.

**Step 6 — Acceptance Seeks** (`renderAcceptance` `:1906`, state 6). Eight buttons: 0/25/50/75/100/50/Home(0)/Full(100) — **four are duplicates**. Each → `/api/wizard/seek?pct=` at `max(fMinimumPower, 30)%`, guarded to `WIZARD_ACCEPTANCE` only (`:2657`). "Looks good — Finish Wizard" → `confirm()` → `/api/wizard/accept` → `location.href='/periscope'`.

**Step 7 — Complete** (`renderComplete` `:1938`). "Go to main control" + "Re-run wizard (clears calibration)". The `rerun()` confirm text says the opposite of the button label: *"Existing calibration is preserved until you re-calibrate"* (`:1946`) vs button *"(clears calibration)"* (`:1943`). `/api/wizard/reset` → `wizardReset()` `.ino:846` → `NOT_STARTED`. Calibration is in fact **preserved**; the button label is wrong.

### Auto-promotion of existing installs
`initWizardStateFromBoot()` `.ino:979-999`, called once from `setup()` at `.ino:6186` after `sSettings.read()`. If `fWizardState >= COMPLETE` → return. Else, if `sSettings.fUpLimitsCalibrated && sSettings.fDownLimitsCalibrated` → silently set `WIZARD_COMPLETE` and save, logging `"WIZARD: existing calibration detected — marking complete (firmware upgrade path)"`. Otherwise logs `"first-run setup required — motor motion gated until complete"`.

### Re-run / skip / bypass — four different doors
1. `/wizard` → Step 7 → "Re-run wizard" (`/api/wizard/reset`).
2. `/wizard` → Step 1 → "Expert mode: skip wizard" (`/api/wizard/skip`).
3. `/calibrate` → motor dropdown → `/api/setmotor` → `changeMotorTypeBypassWizard()` `.ino:954` sets `WIZARD_COMPLETE`. The page copy admits it: *"Bypasses the first-run wizard"* (`WebPages.h:1268`).
4. Serial `#PMOTOR<n>` → `wizardChangeMotorType()` `.ino:5467` → resets to `NOT_STARTED`.

### Wizard problems
- **The EMERGENCY STOP button on the wizard cannot fire during the two steps that need it.** `webServer.handle()` runs on core 0 in `eventLoopTask` (`.ino:6400-6412`). `/api/wizard/creep` (up to 30 s) and `/api/wizard/calibrate` (1-3 min) run *synchronously inside that handler*. While either is running the HTTP server is blocked, so a new `GET /api/estop` cannot be accepted or serviced. Only the serial `:PX` path works — and the cards tell the user twice to "Hit EMERGENCY STOP at any time" (`:1854`, `:1886`).
- Same block starves the 600 ms `/api/wizard/status` poll; with `WifiWebServer<20, …>` (`:2713`) the 20 client slots can fill and the library prints `"NO CLIENTS AVAILABLE"`.
- Step numbering is incoherent: seven progress segments (`:1724`), eight enum states, labels reading "Step 1 of 6" … "Step 5 of 6", no "Step 6 of 6".
- `/api/wizard/setmotor` and `/api/setmotor` both return `"reboot":true` and both are documented "caller must reboot" (`.ino:951`, `.ino:965`) — **no caller ever reboots.** `setMotor()` (`:1818`) just `alert()`s and polls; the calibrate page just calls `loadMotor()`.
- Nothing forces a user to the wizard. `/` → `/periscope`, which contains **zero** wizard awareness (`wizardComplete()` appears only at `WebPages.h:1108`, on `/setup`). On a fresh board the joystick, dial, up/down and random buttons all look live and silently do nothing.

---

# 4. Rescue page (`/rescue`, `printRescuePage` `WebPages.h:813-1033`)

Top-to-bottom inventory:

1. **EMERGENCY STOP** — `button.estop`, full width, `onclick='fetch("/api/estop")'` (`:819`).
2. **Lifter jog card** (`:822-843`)
   - "jog speed" range slider `#jspd`, 0-100, default **80** (`:828`) — no clamp to `maxThrottle`, unlike the periscope slider.
   - `⇧ up` / `■ stop` / `down ⇩` in a `.g3`. Up/down are hold-to-run: `onpointerdown='jogStart(±1,this)'`, `onpointerup`/`onpointercancel='jogStop()'` (`:835-842`). `jogStart` re-sends `/api/lift?t=` every **1500 ms** (`:919`); server expiry is 10 s (`:2238`); `loop()` watchdog stops the motor at `.ino:6615-6618`.
   - `jogStart` uses the bare global `event` for `setPointerCapture(event.pointerId)` (`:917`) — deprecated `window.event`.
   - **Bypasses:** the lifter jog does *not* bypass the descend-when-homed clamp by itself. `/api/lift` (`:2216-2226`) blocks `t<0` when `!rotaryHomeLimit() && position > fRotaryMinHeight` **unless** the override is live.
3. **Rotary shuttle card** (`:845-861`)
   - 180 px canvas `#rd` (220 tablet / 260 desktop, `:931`), left half blue / right half red, drag-x → speed, dead zone < 14 px (`:966-968`). Readout `#rv` (%) and `#rd2` (direction).
   - Re-sends `/api/rotary?s=` every **500 ms** while active (`:974-977`).
   - **Bypasses:** the 40 % `ROTARY_MINIMUM_POWER` floor (`rotaryMotorSpeedForce` `.ino:4324` → `rotaryMotorMove(speed, /*skipSafety*/true)`), and — when the override is live — `rotaryAllowed()` (`:2190-2191`).
   - `■ stop` and **`set home`** (`/api/action?do=sethome`) below it.
4. **Recovery card** (`:863-882`) — six buttons in a `.g2`: `clear fault`, `enable motors`, `home rotary`, `re-run safety`, `set safe spin ht`, `clear encoder`. All fire-and-forget `fetch()` with **no response handling and no confirmation text** — the only feedback is whatever the 500 ms status poll reflects a moment later.
5. **Safety override card** (`:885-899`, `:1017-1030`)
   - Copy: "Hold button 3 seconds to temporarily bypass safety checks (60s)."
   - `#ovr-btn` long-press: `pointerdown` starts a 1 Hz `setInterval` counting `3… 2… 1…` in the button label, then on expiry sets `"activating..."` and fires `fetch('/api/action?do=override_on')` (`:1020-1024`). `pointerup`/`pointercancel` cancel (`:1025-1027`).
   - **The v3.5.3 bug is fixed.** `/api/action?do=override_on` at `WebPages.h:2293` sets `sRescueOverrideExpiry = millis() + 60000`. The handoff note (`docs/REWRITE-HANDOFF.md` §6.3) is now stale.
   - Expiry is read back from `/api/status`'s `override` field (seconds remaining, `:2332-2341`) and drives the red "SAFETIES OVERRIDDEN — Ns remaining" banner + "cancel override" → `override_off` (`:1008-1013`).
   - Also cleared by `/api/estop` (`:2250`).
   - **But the override is only honoured in two places** — `/api/rotary` (`:2191`) and `/api/lift` (`:2219`). It is *not* honoured by `rotaryMotorUpdate()`, which runs every `loop()` iteration on core 1 via `lifter.animate()` (`.ino:6626` → `.ino:3842` → `.ino:2016`). At `.ino:2026-2049` it sees `!rotaryAllowed() && fRotaryThrottle != 0`, prints `"ROTARY STOPPED: …"`, calls `rotaryMotorStop()` and zeroes `fRotarySpeed`. **Core 1 cancels the override-authorised rotary jog within milliseconds of core 0 issuing it.** This is exactly the "two cores both drive the motors" defect in the handoff §5.2, and it makes the rotary half of the rescue page unreliable precisely when it matters.
   - Long-press is **pointer-only**. No keyboard path, no `role`/`aria`, no `touch-action` on `#ovr-btn` — a scroll gesture starting on it can cancel mid-count.
6. **Live status chips** (prefix `c`) + **limit switches** — `printStatusChips(out,"c")` `:902`, six + five chips (see §5).
7. **Definitions box** — `printDefinitionsBox()` `:903`.

---

# 5. Periscope page (`/periscope`, `printPeriscopePage` `WebPages.h:474-807`)

### Six-column status bar (`printStatusBar` `WebPages.h:309-338`)
`#sbar`, `grid-template-columns:repeat(6,1fr)`, sticky top, z-index 100. Cells: **height** (`#st_h`, `d.height+'%'`), **rotation** (`#st_r`, `"home"` when `rotDeg<5 || >355`, else `N°`), **safety** (`#st_s`, FAULT/Ready/WAIT), **motors** (`#st_m`, ON/off), **cmd** (`#st_c`, `d.lastCmd`, ellipsised), **firmware** (static `v3.6.0`, no ID, no element). Colour classes `.c-g/.c-y/.c-r/.c-b/.c-m` at `:55-56`.

### Page contents
1. **Position row** `.pos-row` (`:481-500`)
   - **Lift canvas joystick** `#lc`. Sized once at load from `window.innerWidth`: 60×130 phone / 70×160 ≥640 / 80×200 ≥1024 (`:621-624`). Top half blue (up), bottom half red (down), `▲`/`▼` glyphs, centre line, knob follows the pointer. Throttle formula (`:657-661`): `dir × |py − h/2|/(h/2) × liftSpeed/100`. Re-sends every **1500 ms** (`:674`); release → `/api/lift?t=0`. Value readout `#lv`.
   - **Rotary dial** `#dc`. 160/200/240 px (`:687`). 12 tick marks, green "home" tick + label at 0°, blue needle + hub. Drag to aim, **release** sends `:PA<deg>,<rotSpeed>` (`:739-742`). Readout `#dv`. Needle is re-synced from `d.rotDeg` on every poll while not dragging (`:788-791`). Hint text: "drag & release to set position".
2. **`■ STOP`** — `button.estop` (`:503`) → `/api/estop`. Labelled **"STOP"** here but **"EMERGENCY STOP"** on `/rescue` and the wizard.
3. **lifter** (`:507-513`) — `⇧ up` → `:PP100,<lspd>`; `⇩ down` → `:PH<lspd>` (homes rotary first).
4. **rotary** (`:516-523`) — `↺ spin left` → `:PR<rspd>`; `spin right ↻` → `:PR-<rspd>`; `⌂ home` → `/api/action?do=homerotary`.
5. **random** (`:528-542`) — full-width `🎲 random (default)` → `/api/movemode`; then a `.g4` of `gentle` / `medium` / `aggressive` / `■ stop` at `font-size:13px`.
6. **light kit** (`:545-554`) — eight `.lp` pills, `<div>` not `<button>`, `onclick='setLK(i)'` → `:PL<i>`. Names `full, off, obi wan, yoda, sith, search, dagobah, sparkle` — match `kLightKit_*` 0-7 (`.ino:1254-1264`) exactly. Active pill is re-synced from `d.lightkit` (`:796-801`).
7. **sequences** (`:557-564`) — nine `.sq` `<div>`s labelled 0-8, `onclick='runSeq(i)'` → `:PS<i>`. **These are almost always dead.** `:PS<n>` reads a *stored* sequence from EEPROM (`.ino:4829-4844`); if nothing is stored it sets `sCmdBuffer[0]='\0'` and does nothing. Sequences can only be authored over serial (`#PS<n>:<seq>`, `#PL`, `#PD<n>`) — **there is no sequence editor anywhere in the web UI**. Worse, `runSeq()` (`:758-764`) marks the button `.sq.on` unconditionally, so a no-op looks like a success.
8. **speed** (`:567-579`) — `lift speed` (`#lspd`) and `rotary speed` (`#rspd`) sliders, both default 50. `#lspd`'s `max` is clamped at load from `/api/wizard/status`'s `maxThrottle` (`:599-606`) — a status endpoint for a *wizard* is the source of truth for a slider on the main page.
9. **live status** chips (prefix `p`, `printStatusChips` `:400-443`): height, enc ticks, rot deg, safety, motors, driver fault.
10. **limit switches** chips: top limit, bot limit, rot home, lift fault, rot fault. Note **`lift fault` and `rot fault` are duplicated** — they already feed the "driver fault" chip above (`:461-467`), and `_sw` renders them as green-"ON"/red-"off", so an *active fault* renders **green "ON"** (`:451-453`). That is inverted semantics on a safety indicator.
11. **definitions box** (`printDefinitionsBox` `:384-398`) — a paragraph explaining "Safety" and "Fault", pinned to the bottom of both the periscope and rescue pages. It tells the user faults are "usually cleared by the E-STOP button on the Rescue tab" — but the actual clear-fault control is `clear fault` on the rescue page, not E-STOP.

**Not on this page:** any preset-position buttons (0/25/50/75/100). Those exist only inside the wizard's acceptance step. There is no absolute-height control on the main page other than dragging the joystick, which is a *velocity* control, not a position one.

---

# 6. Parameters page (`/parameters`, `printParametersPage` `WebPages.h:1365-1655`)

| Label | key | Widget | Range | Unit | Default | Backing store |
|---|---|---|---|---|---|---|
| Min lifter power | `lftminpwr` | number | 0-100 | % | 65 Greg / 45 19:1 / 30 IA | `sLifterParameters.fLifterMinPower` |
| Min seek-bottom power | `lftseekbotpwr` | number | 0-100 | % | 40 / 30 / 30 | `fLifterMinSeekBotPower` |
| Min rotary power | `rotminpwr` | number | 0-100 | % | 40 (Greg) / 20 (IA) | `fRotaryMinPower` |
| Lifter distance | `lftdist` | number | ≥0 | ticks | 450 / 1350 / 845 | `fLifterDistance` |
| Safe spin height | `minsafeheight` | slider + "Set to current height" | 0-100 | % | 50 % of distance | `fRotaryMinHeight` (ticks) |
| Drift correction | `driftpct` | slider | 0-20 | % | 5 (`.ino:431`) | `fDriftCorrectionPct` |
| Disable rotary unit | `rotdisabled` | checkbox | — | — | false | `sSettings.fDisableRotary` |
| Random aggressiveness | `aggression` | select | 0/1/2 | — | 1 Medium | `fAggressiveness` |
| Unlock 100% throttle — expert mode | `expert` | checkbox | — | — | false | `fExpertMode` |

**"Set to current height"** (`:1428`, `setSafeSpinFromCurrent()` `:1612`) → `/api/params/safespin_current` → `setSafeSpinHeightToCurrent()` + `save()`, then re-loads and reports `"Safe spin height set to current (N%)."`.

**Load defaults** (`:1506-1519`) — two buttons only: `Greg 6.3:1` (type 0) and `IA-Parts` (type 2). **There is no 19:1 option**, even though `kProfilePololu19` has its own defaults (45/30/1350). `/api/params/defaults` (`:2555`) has no `type==1` branch — asking for it silently loads Greg's numbers. It also resets `fRotaryMinHeight` to `LIFTER_DISTANCE/2` and `fDriftCorrectionPct` to 5 without saying so, and **writes the live running globals immediately** even though the copy says "Save afterwards to persist" (`:1509`).

**Expert card** (`:1480-1503`) — whole card `display:none` unless `d.expertapplies` (i.e. motor type is 19:1). Red-bordered `.card` on `#fffaf9`. Checkbox + a "Frame damage risk" paragraph. The `change` listener (`:1571-1582`) shows a blocking `confirm()` with the full warning and un-checks on cancel. **Re-cal banner** `#expertrecal` (`:1494-1501`): amber, shown when `expert && expertapplies`, text "Existing calibration only covers up to 85%… commands above 85% fall back to the 85% PWM limits", with a `Run calibration →` link to `/calibrate`.

**Save / Back / message** (`:1522-1530`) — `saveParams()` builds a query string by hand (`:1585-1595`) and GETs `/api/params/save`. `#paramsmsg` shows `Saving…` → `Saved.` (green) / `Save failed.` (red). Same element is reused for the defaults and safe-spin messages.

### Two real defects here
- **`lftdist` is inert on any calibrated build.** The page reads and writes `sLifterParameters.fLifterDistance`, but calibration writes `sSettings.fLifterDistance` (`.ino:4389-4404`), and `sSettings.getLifterDistance()` **prefers** the calibrated value. So editing "Lifter distance" changes only a fallback that is never consulted once calibration has run.
- **The safe-spin % uses a different denominator than the status bar %.** `/api/params` computes `safePct = ROTARY_MINIMUM_HEIGHT * 100 / max(1, sLifterParameters.fLifterDistance)` (`:2438-2439`), while `getLifterHeightPercent()` divides by `sSettings.getLifterDistance()` (`.ino:4275-4284`). On a 19:1 whose calibration measured 1200 ticks against a profile default of 1350, a "50 %" safe-spin setting reads as 56 % in the status bar. The Setup page summary row (`:1206-1208`) shares the wrong denominator.

---

# 7. Calibrate, Setup, Log, and the four ReelTwo pages

### `/calibrate` — `WebPages.h:1248-1355`
Renders `printStatusBar(out, "setup")` — so on desktop the sidebar highlights *setup*, not calibrate. Three cards:
1. **motor type** (`:1255-1269`) — `Active: <strong id='motorname'>`, `<select id='mottype'>` with the three motors, `onchange='setMotor()'` → `confirm()` → `/api/setmotor?type=`. Helper text explicitly says it "Bypasses the first-run wizard."
2. **lifter direction** (`:1274-1284`) — `Direction: <strong id='dirstate'>` (Normal / Reversed (inverted)), `Reverse direction` button → `confirm()` → reads `/api/wizard/status` to learn the current value, then `/api/setinvert?value=!v`. Two round trips, no optimistic state.
3. **calibration** (`:1286-1298`) — explanatory paragraph, a red "⚠ Keep clear of the periscope during calibration." line, and `start calibration` → `fetch('/api/cmd?c=%23PSC')` (`:1338`).

`#calmsg` is one shared status line for all three cards. Note the asymmetry: calibration launched *here* goes through `/api/cmd` → `#PSC` → the **serial** dispatcher on core 1, so the web server stays responsive and the STOP path works; calibration launched from the **wizard** goes through `/api/wizard/calibrate` and blocks core 0. Same operation, two completely different concurrency behaviours and two different result-reporting mechanisms ("watch the log page" vs. a JSON `ok`).

### `/setup` — `WebPages.h:1096-1242`
An iOS-Settings-style list of `.set-row` links, each with a coloured `.set-icon`, name, and a live subtitle:

| Row | Subtitle | Line |
|---|---|---|
| first-run wizard | `SETUP REQUIRED — motion blocked until complete` (amber row) or `motor: <name>` | `:1107-1129` |
| calibrate | "run calibration routine" | `:1132` |
| marcduino | `ID: N • baud: N` | `:1144` |
| wifi | `<ssid> • access point\|client\|disabled` | `:1158` |
| remote | `<hostname> • enabled\|disabled` | `:1178` |
| parameters | `min lift: N% • safe spin: N%` | `:1196` |
| firmware | `__DATE__` + optional `sources` link | `:1212` |

**Danger zone** (`:1226-1236`): `clear prefs` (native `confirm('Clear all preferences?')` → `/api/action?do=clearprefs` → redirect to `/setup`) and `reboot` (`confirm('Reboot now?')` → `/api/action?do=reboot`). Both `.danger`. Neither warns that `clear prefs` also wipes calibration and the wizard state.

### `/log` — `WebPages.h:1039-1090`
`clear` (clears the DOM only, not the ring buffer) and an `auto-scroll: on/off` toggle whose *label is its own state* (`:1047-1050`). `#lb` is a dark terminal (`#1a1a2e` / `#4ade80` monospace, `height:calc(100vh - 200px)`, `:187-189`). Polls `/api/log?since=_s` every **1000 ms** (`:1088`), appends new `idx\tmsg` lines. Server side: `WebLogBuffer` `.ino:480-531` — **50 lines**, 120 chars each, tees `DEBUG_PRINT*` to both Serial and the buffer, and suppresses the library's `"New client"` spam (`.ino:539-545`). No filtering, no search, no severity, no download, no timestamps.

### `/marcduino`, `/wifi`, `/remote`, `/firmware`
These are the only pages built from ReelTwo `WElement` builders. **They do not use any of the custom CSS.** `WPage::emit()` (`WifiWebServer.h:1257-1274`) writes its own document with `<style>body { text-align: center; font-family: "Trebuchet MS", Arial; …}` and then each element's `emitCSS`. Controls are raw `<input type='button'>` / `<input type='checkbox'>` with `.<id>_css { width: 300px; }` (`WifiWebServer.h:383, 397`). No status bar, no tab bar, no sidebar, no viewport-fit, centred Trebuchet text.

The comment at `WebPages.h:1954-1956` — *"These use WElement framework for form widgets but we supply custom CSS above"* — **is factually wrong**; `printPageHead()` is never invoked for these routes.

- `/marcduino`: ID # (int), serial baud (2400/9600 select), "Marcduino on WiFi (port 2000)" checkbox, "Last command:" readonly field bound to `sCopyBuffer`, Save, Back.
- `/wifi`: WiFi enabled, Access point mode, Network name, Password, **Save & reboot**, Back.
- `/remote`: Droid remote enabled, Device name, Secret, **Save & reboot**, Back.
- `/firmware`: a `WHTML` block linking to the GitHub releases page (`:2060-2069`), `WFirmwareFile` picker, `WFirmwareUpload` → POST `/upload/firmware`, current version + `__DATE__`, optional sources link, Back. No progress feedback beyond whatever `WFirmwareUpload` draws, and the failure response is the malformed `HTTP/1.0 200 FAIL`.

---

# 8. Asset storage & size

**Everything is `out.print()` of C string literals** — one call per CSS rule, one per HTML fragment, one per JS statement. Roughly **900 `out.print()` calls** across the page functions. No PROGMEM, no `R"RAW(...)"` raw strings in `WebPages.h` at all, no templating, no placeholder substitution, no gzip, no `Content-Length`, no `ETag`. Every page sends `Cache-Control:no-cache` (`:2097` etc.), so nothing is ever cached.

Measured literal bytes per function (unescaped):

| Function | Lines | Bytes |
|---|---|---|
| `printPageHead` (shared CSS) | 21-274 | **8 553** |
| `printStatusBar` | 309-338 | 726 |
| `printTabBar` | 340-358 | 114 |
| `printStatusPollJS` | 361-380 | 611 |
| `printDefinitionsBox` | 384-398 | 642 |
| `printStatusChips` | 400-443 | 1 128 |
| `printUpdateChipsJS` | 447-468 | 843 |
| `printPeriscopePage` | 474-807 | 8 584 |
| `printRescuePage` | 813-1033 | 7 114 |
| `printLogPage` | 1039-1090 | 1 554 |
| `printSetupPage` | 1096-1242 | 3 827 |
| `printCalibratePage` | 1248-1355 | 4 361 |
| `printParametersPage` | 1365-1655 | 11 164 |
| `printWizardPage` | 1665-1951 | 12 788 |
| **whole file** | | **68 437** |

Tab icon macros (`WebPages.h:276-294`): PERISCOPE 204 B, RESCUE 213 B, LOG 278 B, SETUP 249 B = **944 B, emitted twice per page** (sidebar + tab bar) because both bars are always rendered.

**Approximate wire size per page load** (shell = head 8 553 + status bar 726 + icons 944 + tab bar 114 + icons 944 ≈ **11.3 KB**):

| Page | ≈ bytes |
|---|---|
| `/periscope` | ~22.5 KB |
| `/parameters` | ~22.4 KB |
| `/rescue` | ~21.0 KB |
| `/setup` | ~15.7 KB |
| `/calibrate` | ~15.6 KB |
| `/log` | ~12.8 KB |
| `/wizard` | ~12.8 KB (standalone; own 2 KB CSS block, no shell) |

**Flash cost of the web layer: ~68 KB of string literals in `WebPages.h`**, plus the 2 100-byte SVG in `web-images.h`, plus the ReelTwo `WElement` machinery. Against the ~94 % program-storage figure in `CLAUDE.md`, that is roughly 5 % of total flash spent on markup — most of it the same 8.5 KB of CSS compiled once but transmitted on every navigation.

**Images:** the only web image asset is `WSVG rseriesSVG` in `web-images.h:6-42` — a single 64×64 R-series droid glyph, 2 100 bytes of inline SVG path data in a `R"RAW(...)"` literal. **It is never referenced.** A repo-wide grep for `rseriesSVG` returns exactly one hit: its own definition. All icons actually shown are the four inline `TAB_ICON_*` SVGs plus ~7 more one-off inline SVGs hand-written into `printSetupPage`. Nothing is a binary image; nothing is gzipped.

**JSON:** hand-built with sequential `out.print()` calls. No ArduinoJson. `lastCmd` escaping (`:2343-2346`) handles only `"` — a backslash or control byte in `sCopyBuffer` produces invalid JSON and silently kills every `.then()` on the page.

---

# 9. Known-broken / dead / inconsistent paths

**Dead code and assets**
- `rseriesSVG` (`web-images.h:6`) — defined, never used. `web-images.h` exists solely to hold it.
- `bool sOTAInProgress` (`WebPages.h:15`) — written at `:2690` and `:2694`, **never read anywhere** in the project. Nothing suppresses motor motion during a firmware flash.
- Dead CSS in `printPageHead`: `.back-link` (`:178`), `.form-actions` + `.form-actions button` (`:176-177`), `hr{}` (`:184` — zero `<hr>` in the file), `input[type=password]` rules (`:165-171` — no password input exists on any page that loads this CSS). All of these were for the WElement sub-pages, which don't load it.
- `WIZARD_FIND_MIN_POWER` (state 4) is never a distinct UI step and `WIZARD_CALIBRATION` (5) is never set — `/api/wizard/calibrate` jumps 4 → 6 (`:2620`).
- `LifterSettings::fSafetyManeuver` bitfield is commented `// (unused)` (`.ino:~880`).

**Broken or misleading behaviour**
- **Wizard ESTOP is unreachable during creep and calibration** (see §3). The single worst issue in the UI.
- **`rotaryMotorUpdate()` on core 1 cancels the rescue safety override** within one `loop()` iteration (§4).
- **Sequence buttons 0-8 are no-ops on any board without serially-stored sequences**, and light up as if they worked.
- **The `lift fault` / `rot fault` limit-switch chips display an active fault as green "ON"** (`printUpdateChipsJS` `:451-453`, `:466-467`) while the "driver fault" chip immediately above shows the same data as red "FAULT" (`:461-462`).
- `roundRect` with an object radius: `_lctx.roundRect(1,1,w,h,{upperLeft:13,upperRight:13})` (`:637-642`) is the **non-standard pre-spec API**. Per the current spec `radii` must be a number, `DOMPointInit`, or array — an object with `upperLeft` keys is coerced to a zero-radius `DOMPointInit`, so the rounded corners silently don't render.
- More seriously, `roundRect` itself is Safari 16 / Chrome 99+. On older iOS it is `undefined`, `drawLift(null)` at `:653` throws, and **the entire inline `<script>` aborts** — joystick, dial, light kit, sequences, and the status poll all die with no error shown. There is no `try`/`catch` and no feature detection anywhere on the page.
- `/api/params/defaults` mutates live motor-control globals but the UI says "Save to persist" (`:1509`, `:1633`).
- `/api/lift` sets `sSafetyManeuver = true` (`:2235`) — a raw jog asserts that the startup safety maneuver *succeeded*, flipping the status bar from WAIT to Ready without any maneuver having run.
- `/api/action?do=setsafeheight` (rescue) does **not** persist; `/api/params/safespin_current` (parameters) does. Two buttons, same operation, different durability.
- `"reboot":true` is returned by `/api/setmotor` and `/api/wizard/setmotor` and ignored by both callers.
- `/api/action` returns `{"ok":true}` for unknown verbs.
- Wizard "Re-run wizard (clears calibration)" vs its own confirm text "Existing calibration is preserved" (`:1943` vs `:1946`) — the confirm is correct, the label is wrong.
- Creep-test copy says "30% throttle" (`:1853`); the code computes `max(0.30 × ceiling, defaultMinPower)` = 0.45 for the 19:1 (`.ino:3450-3452`).
- `printDefinitionsBox` tells users to clear faults with E-STOP; the actual control is `clear fault`.
- Creep-test completion lives in `window._creepOk` — lost on reload (`:1867`).
- `/calibrate` marks the *setup* tab active (`:1251`); `/parameters` and `/wizard` likewise or not at all.

**Window-size-dependent inline script**
- `var _vw = window.innerWidth` is read **once at load** on `/periscope` (`:621`) and `/rescue` (`:930`), and every canvas dimension derives from it. There is no `resize`/`orientationchange` handler. The CSS breakpoints *do* respond live (`:215-271`), so after a phone rotates to landscape the `.pos-row` grows to 240 px while the canvases stay at their 130/160 px portrait size — the joystick's `_lcH` no longer matches its rendered box, and because throttle is computed from `_lcH` (`:658`), the *hit geometry is wrong*, not merely the visuals.

**Protocol-level**
- HTTP/1.0, `Connection: close`, no `Content-Length`. Every poll is a fresh TCP connection: `/rescue` alone opens 2 status connections/sec plus 2 rotary keepalives/sec. `WifiWebServer<20, …>` (`:2713`) has 20 slots and stops each client after one response.
- Every state-changing operation is a **GET** — `/api/estop`, `clearprefs`, `reboot`, `override_on`, firmware motor change. Any prefetching browser, link scanner, or `<img>` on the same network can fire them.

---

# 10. Design-system critique

### What the system actually is

**Colour.** 52 distinct hex literals hand-typed across `WebPages.h`, no variables, no `:root` custom properties. The core set is recognisably the Tailwind palette copied by eye — `#2563eb` blue-600, `#dc2626` red-600, `#16a34a` green-600, `#d97706` amber-600 — but it is joined by a long tail of one-offs: `#b00` for the override banner (`:893`), `#0a58ca` for one link (`:2064`), `#1a1a2e`/`#4ade80` for the log terminal (`:187`), `#1a1a1a`/`#9ae6b4` for the *wizard's* log terminal (`:1714`), `#fffaf9`, `#7c2d12`, `#92400e`, `#78350f`, `#7f1d1d`, `#166534`. Two different dark-terminal themes for the same concept. Neutrals alone span nine values (`#f0f2f5, #f7f8fa, #fafbfc, #f4f6f8, #e8eaed, #e8eaf0, #e4e6ea, #dde1e7, #cdd1d9`) with no discernible scale.

**Typography.** Five stacks: `'Helvetica Neue',Helvetica,Arial,sans-serif` (main, `:33`), `-apple-system,Helvetica,Arial,sans-serif` (wizard, `:1671`), `"Trebuchet MS", Arial` (the four ReelTwo pages, `WifiWebServer.h:1264`), `monospace` (log, `:187`), `ui-monospace,Menlo,monospace` (wizard log, `:1713`). Sizes run 9, 10, 12, 13, 14, 15, 16, 17, 18, 20 px with no scale and heavy use of inline `font-size:` overrides that defeat the class system entirely.

**Spacing.** No scale. Gaps of 2, 3, 4, 5, 6, 7, 8, 10, 12, 14 px; paddings of 6/4, 8/4, 8/6, 8/10, 8/14, 9/10/7, 10/8, 10/12, 10/14, 11/14, 12/0, 12/14, 14, 14/8, 16, 18. Margins are written inline (`style='margin-bottom:10px'`) on roughly a third of the cards.

**Layout.** Server-rendered strings, no components. Grid helpers `.g2/.g3/.g4/.g5` (`:82-85`) plus ad-hoc `display:flex` written inline. Breakpoints: **640 px** (`:215`) and **1024 px** (`:229`). Only three things actually change at 640: chip grid 3→5 columns, `.g5` 5→9, `.pos-row` height. At 1024 the tab bar swaps for the sidebar. There is no intermediate treatment — a 768 px tablet gets the phone's bottom tab bar with a 6-cell status bar and 9-across sequence grid.

### Touch targets
iOS HIG minimum is 44×44. Measured:
- `.lp` light-kit pills — `padding:8px 14px; font-size:14px` ≈ **33 px tall**, and they are `<div onclick>`, not buttons.
- `.sq` sequence tiles — `padding:12px 0; font-size:16px` ≈ **43 px**, also `<div onclick>`.
- Parameters number inputs — `padding:8px; font-size:14px` ≈ **34 px**, and only 80 px wide.
- `input[type=range]` thumb — **22 px** (`:128`).
- `.g4` random row on a 375 px phone: `(375 − 28 − 18)/4 ≈ 82 px` per cell for the word "aggressive" at 13 px — it will not fit.
- `#sbar` on a 375 px phone: six cells at `(375 − 20)/6 ≈ 59 px`, minus `.ss` padding = **~51 px of content** for `.sv{font-size:14px;font-weight:700}`. "FAULT" and "v3.6.0" are right at the edge; at 320 px they clip. The `cmd` cell is already explicitly ellipsised (`:321`) because it never fits.
- What *is* right: `button` at `padding:14px 8px` ≈ 50 px, `.set-row` at ~64 px, `.tab` at ~44 px, `button.estop` at ~57 px.

### Contrast (WCAG AA needs 4.5:1 for text, 3:1 for UI)
- `.tab-lbl{color:#aaa}` at **10 px** on white (`:204`) — **≈2.1:1**. This is the label on the primary navigation, and it is the *inactive* state, i.e. three of four tabs at all times.
- `.set-arr{color:#bbb}` at 20 px (`:159`) — **≈1.9:1**.
- `.sl{color:#888}` at **9 px** on `#f7f8fa` (`:53`) — **≈3.3:1**. These are the labels for every status-bar cell and every chip.
- `.dial-hint`, `.lift-lbl`, `.spd-lbl`, `.sec`, all the `13px color:#888` helper paragraphs — **≈3.1-3.3:1**.
- `.c-y{color:#d97706}` for the **WAIT** safety state on `#f7f8fa` — ≈3.3:1, and it is one of the three states of the single most safety-relevant readout.

### Semantics and accessibility
Counted across `WebPages.h`: **0** `aria-*` attributes, **0** `<label for=…>`, **0** `alt=`, **0** `title=`, **0** `role=`. Seven `<h1>/<h2>` total, all inside `/wizard`; the four main pages have **no heading elements at all** — `.sec` divs are visual-only. The two canvases have no accessible name, no fallback content, and no keyboard equivalent. The light-kit pills and sequence tiles are `<div onclick>` — not focusable, not activatable by keyboard, not announced as controls. The 3-second override long-press is pointer-only. Every state indicator is colour-plus-text, which is the one thing done right.

### Feedback and error handling
Three incompatible patterns coexist:
1. **Fire-and-forget** — `fetch(url).catch(function(){})`. Every button on `/periscope`, all six recovery buttons and `set home` on `/rescue`, the tab-bar links. A failed request is indistinguishable from a successful one; a *blocked* request (`{"ok":false,"blocked":"lift"}`) is also indistinguishable, because nobody reads the body.
2. **Shared status line** — `#calmsg` (calibrate) and `#paramsmsg` (parameters), one `<div>` reused for save/defaults/safe-spin/invert results, colour-coded green/red.
3. **Native `alert()` / `confirm()`** — wizard motor apply (`:1821`), wizard skip, rerun, accept; parameters expert-mode, load-defaults; calibrate motor change, invert; setup clear-prefs, reboot. Nine blocking browser dialogs, styled by nothing.

Nothing anywhere shows a spinner or disabled state during a slow operation except `#rc` in the wizard (`:1896`). The `busy` flag in the wizard is a bare boolean with no timeout — if a `fetch` rejects in a way the `.catch` misses, the wizard latches busy forever.

### Safety-critical controls
- **ESTOP** is genuinely well distinguished: `button.estop` (`:75-79`) is solid `#dc2626`, white, 18 px/700, 18 px padding, full width, 12 px radius. It is the only solid-red-fill control in the system. But: it is labelled "STOP" on `/periscope`, "EMERGENCY STOP" on `/rescue`, "EMERGENCY STOP" on `/wizard`; it is *inline* (scrolls away) on `/periscope` and `/rescue` but `position:fixed;bottom:0` on `/wizard` (`:1689-1692`); it is **absent entirely** from `/setup`, `/calibrate`, `/parameters`, `/log`, and the four ReelTwo pages — including `/calibrate`, which starts a multi-minute full-travel motion.
- **Safety override** is the second-best-distinguished control: `.danger` outline, ⚠, an explicit 3-second hold, a live countdown in the label, and a solid `#b00` banner while active. Good design, undermined by the core-1 race.
- Everything else at the same hazard level is a plain outline button: `clear fault`, `enable motors`, `re-run safety`, `clear encoder`, `set home`, `start calibration`, `Reverse direction`, `clear prefs`, `reboot`. `clear prefs` and `reboot` get `.danger` and a `confirm()`; `clear encoder` and `set home` — which destroy the rotary position reference — get nothing.

### Audience mixing — the core of the "kitchen sink" complaint

Three distinct users are interleaved on the same screens:

| Audience | What they need | Where it actually is |
|---|---|---|
| **Operator at an event** | up, down, spin, random, light kit, presets, STOP | `/periscope` §1-8 — but buried under speed sliders, 11 diagnostic chips, and a definitions essay |
| **Builder on the bench** | motor type, invert, calibration, min-power, distances, safe height, drift, expert mode, log | split across `/setup` → `/calibrate` + `/parameters` + `/wizard`, with three separate paths to change motor type and two to set safe height |
| **Emergency rescue** | jog past the clamps, override, clear fault, home | `/rescue` — correctly separated, but it *also* carries the same 11 chips and the same definitions box as `/periscope` |

`/periscope` is the clearest offender. Above the fold it is an operator page. Below the fold it becomes a diagnostics dashboard: six live-status chips, five limit-switch chips (two of which duplicate two of the six, with inverted colour semantics), and a 640-character paragraph defining "Safety" and "Fault". A builder needs `enc ticks`; an operator at a con has no use for it and no way to hide it. The comment at the top of the file records the decision — *"Live status + switches duplicated into the Periscope tab (bottom)"* (`WebPages.h:10`) — which is exactly how kitchen-sink pages happen: the diagnostics were added to the rescue page for a reason, then copied to the main page "since they're useful", and never removed.

The second offender is **speed**. There are four independent speed controls with no shared model: `#lspd` and `#rspd` on `/periscope` (default 50, `#lspd` clamped to `maxThrottle`), `#jspd` on `/rescue` (default 80, **unclamped**), the per-command speed argument every `:P*` accepts, and the three aggressiveness levels. The joystick multiplies its position by `#lspd`, so dragging fully to the top at the default gives 50 % throttle — the joystick's full deflection means nothing on its own. Nothing on screen explains this; the reasoning is only in a source comment (`:655-656`).

The third is **duplication of function across pages**: motor type is changeable from `/wizard` *and* `/calibrate` with different side effects on wizard state; safe-spin height from `/rescue` (non-persistent) and `/parameters` (persistent); calibration from `/calibrate` (async, via serial, log-reported) and `/wizard` (blocking, via HTTP, JSON-reported); "home rotary" from `/periscope` and `/rescue`.

### Structural duplication in the source
The status-bar update block — `_sc('st_h',…)` through `_sc('st_m',…)` — is **copy-pasted six times** with six different polling intervals (`:369-376`, `:778-786`, `:1061-1068`, `:1344-1351`, `:1644-1651`, plus the shared `printStatusPollJS`). The `_sc()` helper itself is emitted from five places. The four tab icons are emitted twice per page. Any change to the status bar means editing six copies — which is exactly why the rescue page polls at 500 ms and the log page at 2000 ms with no stated reason.

### Bottom line for a rewrite
The strongest ideas worth carrying over: the four-tab information architecture; the six-cell always-visible status bar; the physical-metaphor canvas controls (they suit a mechanism); the deliberately separate rescue page; the 3-second-hold override with live countdown; the wizard's mandatory-acknowledgement checklist; and `/api/status` as a single polled state document.

The things that must not survive: one CSS blob re-sent uncached on every navigation; four pages rendered by a different framework with a different font and no chrome; a wizard whose emergency stop is blocked by the operation it is meant to stop; two cores fighting over the rotary; diagnostics welded onto the operator page; nine `confirm()` dialogs standing in for a feedback system; and 52 hex literals where a dozen tokens would do.
