# Phase 4 brief — full web UI

**Status 2026-09-27 (night): implemented** on branch `claude/uppity-spinner-phase-4-1e5680`
(what was built and where it deviates: `docs/REWRITE-PLAN.md` §5, "Status 2026-09-27 (night)");
the bench procedure is `docs/BENCH-PHASE4.md`. This brief is kept as the record of what Phase 4
started from.

For the next session. Read `CLAUDE.md` first (hard rules, definition of done), then this file,
then `docs/REWRITE-PLAN.md` §4 (the page designs) and §5 (the phase table). The bench docs
`docs/BENCH-PHASE1.md` … `BENCH-PHASE3.md` hold every number measured on Todd's periscope and
every fix that came out of the bench; the "Results" sections are the ground truth.

## Where things stand (2026-09-27)

Phases 0–3 are done and verified on Todd's 19:1 periscope. The board runs the motion core, the
command layer with persistent settings, and the network layer: its own access point
(`R2Uppity-FB3F`, password `Astromech`, `192.168.4.1`), a stored home network it joins in the
background (name `WordNet`; the password is stored on the board and is never printed or written
down), `http://uppity.local`, one web page (Operate + console), a JSON API, a 16 KB log ring, an
E-STOP that lands mid-move, and over-WiFi updates. Latest commit on the session branch:
`7b35682`, build `0551078b`. Host tests: 116. Flash: 1,138,807 bytes of 1,966,080 (57 %).

Todd's numbers (all persisted, nothing to re-send after a flash): 19:1 profile, inverted,
travel 1150 (`#PTRAVEL`; calibration measured 1179), safe-spin 904 ticks, 1185.5 ticks/rev,
home park 1, wiggle 2, unlocked (`#PCOMMISSIONED=1`), `#PMINUP/#PMINDOWN` 50 % from the v3
import, `#PRANDOMLIGHT=7`. Rotations happen as low as 908 ticks; `#PSAFESPIN920` would add
margin (Todd's call, not yet made).

## Lessons from Phase 3 that shape Phase 4

- **The ESP32 web server handles one connection at a time** and, unpatched, holds a connection
  whose request never arrives for 5 s. `Net::dropStalledClient()` cuts that to 400 ms. Keep
  every handler short; never sleep or wait in one; big responses (a page) are fine.
- **The radio loses packets around lifter moves** (pulsed drive current). Polls time out at
  2 s and never stack; the page treats the board as `NO LINK` after three misses. Anything
  safety-relevant must retry on fresh connections until the status confirms it, the way the
  E-STOP button does (`fireEstop()` in `webui/index.html`).
- **E-STOP has its own listener on port 81 in its own task** (`Net::serviceEstopPort`): any
  request mentioning "estop" latches. The page fires four channels (fetch and image GET to port
  81, fetch and sendBeacon to `/api/estop`) on `pointerdown` and `click` and shows a send
  counter. Do not route anything else through port 81, and do not touch this without a bench.
- **A new command interrupts a running seek or rotation** (`isRetargetable`), never a home,
  characterisation or calibration; after any interrupted job the supervisor waits until both
  counts are still (30 ms, 400 ms max) before the next program. Jog will build on this.
- **Flash writes only while disarmed**, through `Persist`; a request while armed prints
  `PERSIST=deferred` and lands within ~2 s of the drivers switching off. Never call NVS from a
  handler.
- **Settings are append-only** (schema 3, `Settings.h`): new fields are 1-byte types appended
  at the end, size stays a multiple of 4, bump the schema, migration in `migrateSettings`.
- **`#PCONFIG` is replayable**; every builder-settable value has a KEY in `kSettingKeys` or
  `kStringKeys`, and `/api/config` GET/POST mirror them. Passwords are never sent to a client.
- **The page** is one file, `webui/index.html`, inline CSS/JS, Sabé tokens, paper theme by
  default with a dark toggle (`#PTHEME`), 44 px targets, `html{font-size:20px}`; regenerate
  `WebAssets.h` with `python3 webui/build-assets.py` after every edit; `webui/dev-server.py`
  fakes the device at `http://127.0.0.1:8765`. Claude's in-app browser could not open localhost
  in the Phase 3 session; syntax-check the script with `node --check` and cross-check element
  ids (see the Phase 3 session pattern), then let Todd see it on the phone.
- **Versioning** is the ELF hash (`Console::buildId()`), printed at boot, in `#PSTATUS` and in
  `/api/state.build`. `__DATE__` stamps lie under the build cache.

## The API as implemented (Net.cpp)

| Route | What |
|---|---|
| `GET /` | the page (gzip, ETag) |
| `GET /api/state` | snapshot + console extras (`random`, `seq`, `light`, `safeSpin`, `travelSet`, `persist`, `theme`, `build`, `inputs`), `log` (ring offset), `wifi` (ap, clients, sta, ssid, ip, rssi, estopPort, estopHits, webEstops, stalled) |
| `POST /api/cmd` `line=<:P or #P line>` | queued to loop() like a serial line (source Web) |
| `POST/GET /api/estop`, `:81/estop` | latch; no auth, no queue |
| `POST /api/estop/reset` | release |
| `POST /api/stop` | soft stop: random off, chain cleared, job cancelled |
| `GET /api/config`, `POST /api/config key=&value=` | all keys; POST queues `#P<KEY>=<value>` |
| `GET /api/log?since=<offset>|tail` | `{from,next,text}`, 3 KB chunks, honest about gaps |
| `POST /api/restart`, `POST /update` (app .bin) | restart; OTA (refused while armed) |

Everything mutating goes through the command queue into loop(), so every page action is also a
serial line and lands in the log. New pages should keep to that: prefer a new `#P` command over
a new endpoint unless the data is bulky or needs a lease (jog).

## Phase 4 scope (plan §4/§5), in the order worth building

1. **Shell and navigation**: phone bottom tabs / desktop left nav for Operate, Setup,
   Diagnostics, Rescue (Sequences last; Todd does not use them). Sticky banners exist for
   STOPPED, FAULT, LOCKED, OVERRIDE, UPDATE. Keep the E-STOP fixed on every page.
2. **Setup → Tuning, Motor, Serial, WiFi, Firmware, Theme**: forms over the existing keys
   (`/api/config`), each field with inline confirmation from the `#PKEY=… (saved)` log line;
   WiFi = SSID/password fields (write-only) + restart; Firmware = version/build, a file picker
   posting to `/update` with progress, restart, factory reset behind a two-step confirm;
   calibration `#PSC` with live progress (add the calibration phase to `/api/state`).
3. **Diagnostics**: live limits/encoders/DIAG/confidences/tick stats (already in the state),
   `#PROTARYTEST` runner with the report lines, the log with filter/pause/download,
   `#PCONFIG` dump as copyable text.
4. **Rescue**: E-STOP/reset; **safety override** (60 s lease: `CommandQueue::setOverride`,
   shown as a countdown; needs a `#POVERRIDE` command or an endpoint); **hold-to-jog** for the
   lifter (`LifterCreep` with the jog lease, `CommandQueue::jogKeepalive`, 2 s lease, the page
   re-sends every 500 ms while held; plan: continuous drive, capped to the profile) and the
   rotary (`RotaryJog`, 12 % floor, override only); set rotary home here; clear fault; unlock
   with acknowledgement. Every destructive action a two-step inline confirm.
5. **Commissioning (Setup → Commissioning)**, plan §4, six steps, state on the device
   (`commissioned` 0/1/2), resumable. One deviation from the plan, from the 2026-09-19 crash:
   the safe-spin height is **measured**, never assumed. Step: head turned 90° at the top, then
   the mast lowered in small steps by the operator with E-STOP ready until the head is judged
   clear, then "use this height" stores `#PSAFESPIN`. Until it is set only the top counts.
   Rotary characterisation runs at the top. The expert bypass (`#PUNLOCK`) stays on this page
   and nowhere else.
6. **Sequences**: parked unless Todd asks; the API and storage exist (`#PS`, `#PL`, `#PD`).

Verification per the plan: commissioning from a factory reset on hardware; rescue override
honoured for the full 60 s; dark theme review. Write `docs/BENCH-PHASE4.md` in the same voice
as the earlier bench docs: one line at a time, real paths, what success looks like.

## Todd's workflow (he is not a coder; every instruction is a complete copy-paste command)

Work lands on a session worktree branch; Todd fast-forwards `v4-rewrite` in the main checkout
himself (Claude's merge attempts are blocked by the tool policy):
```
git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only <session branch>
```
Then, with the periscope idle and the laptop on the house WiFi:
```
cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.4/tools/espota.py -r -i 192.168.86.90 -p 3232 -a Astromech -f ~/Desktop/uppity-build/UppityFirmware.ino.bin
```
(`192.168.86.90` is the board on the house network; `192.168.4.1` on its own network; the
address is on the `#PSTATUS` line. USB flashing works as before if OTA fails.) The monitor:
```
arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-log.txt
```
The monitor echoes typed lines, so a typed password lands in the log file; never repeat one.

## Open items carried forward

- Two phones on the page at once; the command UART (Sabé bus, 9600); the trigger inputs: not
  yet tested. `#PSAFESPIN920` decision. A device PIN for mutating routes (backlog). Bulk
  capacitance near the ESP32 if the radio keeps losing packets during lifter moves. Home
  network retry pauses while a phone is on the AP (Sabé's rule); it still joined within ~90 s.
- Phase 5 (mesh): heartbeat tag `&PERI` vs `&UPPITY` is Todd's call before it starts.

## Paste this to start the next session

    Continuing the Uppity Spinner ALT v4 rewrite. Phases 0-3 are bench-verified on my periscope.
    Read CLAUDE.md first, then docs/PHASE4-BRIEF.md, then docs/REWRITE-PLAN.md section 4, then
    memory project_v4_rewrite and project_todd_periscope_hardware. Start Phase 4 (the full web
    UI) in the order the brief gives: shell and navigation, Setup pages over the existing
    settings keys, Diagnostics, Rescue with the 60 s override and hold-to-jog, then
    commissioning with a measured safe-spin height; sequences stay parked. Keep the definition
    of done from CLAUDE.md, write docs/BENCH-PHASE4.md for me in the same voice as the earlier
    bench docs with complete copy-paste commands (real paths, port /dev/cu.wchusbserial110,
    board 192.168.86.90 on the house WiFi, OTA via espota.py), commit on the session branch and
    give me the merge line, and never script a move that depends on a clearance you have not
    confirmed on my hardware. I am not a coder.
