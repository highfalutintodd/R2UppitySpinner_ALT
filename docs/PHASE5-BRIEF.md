# Phase 5 brief — mesh and DroidNet

For the next session. Read `CLAUDE.md` first (hard rules, definition of done), then this file,
then `docs/REWRITE-PLAN.md` §1.1 (the mesh decision), §3 (where `Mesh.cpp` sits and the task
table), §5 (the phase table) and Appendix C (Sabé conventions, the `&` protocol, the WCBClient
constraints, DroidNet and the Command Library). `docs/v3-inventory/ECOSYSTEM.md` has the local
paths and versions. The bench docs `docs/BENCH-PHASE1.md` … `BENCH-PHASE4.md` hold every number
measured on Todd's periscope; their "Results" sections are the ground truth.

## Where things stand (2026-10-04)

Phases 0–4 are done and verified on Todd's 19:1 periscope, the last of them on 2026-10-04: four
web pages (Operate, Setup, Diagnostics, Rescue), firmware upload from the page, the rescue tools
(60 s override, hold-to-jog, `#PNUDGE`, `#PSETHOME`) and commissioning, run end to end with the
dome on. A full code review followed (`docs/CODE-REVIEW-2026-10.md`, branch
`claude/code-review-2026-10`, merged into `v4-rewrite`): 27 defects fixed plus Todd's D1-D4, and
bench-passed on the droid the same day. **Build on the droid: `7d93e1d8`. Host tests: 202. Flash:
1,181,615 bytes of 1,966,080 (60 %).** Start the Phase 5 worktree from `v4-rewrite`.

Todd's numbers (all stored on the board; nothing to re-send after a flash): 19:1 profile,
inverted, travel 1137 between the switch closings (calibrated, `#PTRAVEL0`), the mast rests up
to 37 ticks below the bottom switch's closing point (`rest=37`, used at power-up), **safe-spin 878,
measured with the dome on** (2026-10-04; it sits ~15 ticks on the safe side of what commissioning
intended, leave it), 1185.0 ticks/rev, home park 1, wiggle 2, `COMMISSIONED=2`,
`#PMINUP/#PMINDOWN` 50 % from the v3 import, `#PRANDOMLIGHT=7`. House network `WordNet` (the
password is stored on the board and is never printed or written down); the board's house address
is DHCP and has been `192.168.86.69` lately (the `WIFI joined` boot line shows it).

**Heartbeat tag: `&PERI`** (Todd's decision, 2026-10-04).

## What the code review changed that Phase 5 must keep

- **Attended vs unattended sources.** The rescue override and the commissioning unlock now apply
  only to moves from `SerialUsb`, `Web`, `Commission` (and `Test`); random mode passes the
  commissioning unlock only (`MotionSupervisor::attendedSource`). **`ReqSource::Mesh` is
  unattended**: a mesh `:PP0` during a rescue never gets the override. Keep it that way, and keep
  `#POVERRIDE`/`#PSETHOME` refused from the mesh (`Console::onRescue`).
- **Stop is `CommandQueue::cancelAll()`**: it ends the running job and refuses everything queued
  (`REFUSED CANCEL`). A mesh `&STOP`-style verb, if any, uses the same call; `&ESTOP` still goes
  only to `EstopLatch::trip`.
- **Intake holds have owners** (`kHoldPersist`, `kHoldOta`, `kHoldRestart`). Anything new that
  must pause motion intake gets its own bit.
- **Restarts wait** for no job, drivers off and the save landed (`Console::requestRestart`). A
  mesh-triggered restart, if one is ever added, goes through it.
- **Serial lines:** a lost byte spoils its line (`kLostBytes`), over-long lines are dropped,
  other boards' `:` lines are silent, and `#PBAUD`/`#PCMDTX` re-register the receive hooks
  (`SerialFront::rehook`). Mesh input should follow the same rules: one line per message, foreign
  lines silent, never `INVALID` for another board's traffic.
- **Settings:** a forced v3 import keeps measured values; a settings blob from a newer schema is
  read and not written back. Phase 5's new fields are appended with a schema bump as usual.
  D8 from the review (`#PCONFIG` does not say whether ticks/rev was measured) is a good fit for
  Phase 5's settings work.
- **Tests:** `test_review.cpp` holds the review's regression tests; the rig has a ground-truth
  safety watch (`Rig::watchSafe`) that any new mesh-driven motion test should switch on; UBSan
  reports now fail `make test`.

## Lessons from Phase 4 that shape Phase 5

- **Safety paths the simulator cannot see go untested.** E-STOP disabled the drivers at once and
  the mast free-wheeled down, because the simulator modelled the screw as self-locking (it now
  has a back-drive model). A mesh E-STOP must land on the same latch as every other source
  (`EstopLatch::trip`, from any context) and nothing else; test it on the simulator and bench it.
- **A trust label is not the sensor.** The rotary's SwitchLive label lags the switch by a 400 ms
  hold at rest; "is the head home" must read the switch itself (fixed in `rotaryIsHome`). Any new
  input that gates motion (mesh commands included) must look at current state, not a filtered one.
- **Every position reference uses the same point**, the switch closings: 0 at the bottom switch,
  the travel at the top switch, the calibration's end, and power-up. The `&PERI` height must be
  derived from the same count, as a share of the travel.
- **Bench steps never rely on a refusal next to an unverified clearance** (hard rule 12). A mesh
  command that turns the head is a move like any other: stage mesh tests with the head home or
  above 878, never at a height nobody measured.
- **Log lines are for Todd.** Plain words, the numbers he needs (`… came to rest …`), one line per
  event; nothing a builder would read as an error when it is not.
- **Installs**: the page's own firmware upload (Setup → Firmware) works and is Todd's preferred
  path; USB (`arduino-cli upload … -p /dev/cu.wchusbserial110`) is the fallback. An upload pauses
  the control loop up to 10 ms at a time with the motors held; that is expected.
- **Everything the board emits must be a silent no-op elsewhere** (hard rule 7). `&PERI,…` lines
  go on the mesh only; `KEY=VALUE` on serial; unknown inbound `&` verbs get no answer.

## Part A of Phase 5: D5, seek braking (do this first)

Todd's decision, 2026-10-04: the code review's D5 is built at the start of Phase 5, benched as
Part A of `docs/BENCH-PHASE5.md`, before any mesh work. It is a change Todd will notice (short
moves now move and land close to where they were sent), and he has approved it.

**The defect.** A seek brakes when it is within `stopComp` of its target from the first tick
(`src/core/LifterAxis.h:81`, `pos <= target + stopComp` going down), and `stopComp` is the
calibrated run-on for the seek's *full* speed slot (`src/core/MotionSupervisor.cpp`, in
`startStep`'s `LifterSeek` case, `p.stopComp = … overshoot[slot]`): 56 ticks down and about 30 up on
Todd's lifter. A step shorter than that never moves (942 → 909 on 2026-10-04); longer short steps
stop up to that much early (bench 2026-10-04: 284 asked, 301 reached; 22 asked, 41 reached).

**The change.**
1. Brake on the run-on at the speed actually reached: the lifter's speed over the last few ms (the
   creep's `kVelWindow` approach) times the braking time the calibration table gives for that speed
   (`overshoot[slot] / vel[slot]`, from the same `MotionConfig` arrays; today `brakeLeadMs()` at
   `LifterAxis.h:270` takes the median over all slots). No table: unchanged, brake on the mark.
2. When the seek's target is the safe-spin floor (a `Verdict::Clamp` from `checkLifterSeek`), add
   the creeps' safe-side margin (`ahead + ahead/4 + 1`, `LifterAxis.h:222`), so a turned head comes
   to rest at or above the floor, never below it (the review's A5).
3. Full-travel seeks (to a limit switch) are unchanged: they end on the switch.
4. If the speed-matched braking time is the same small helper, the creeps can use it too (that is the
   backlog item "nudges land a few ticks short going up"). Only if the tests show it is the same
   change; otherwise leave the creeps alone.

**Tests that must fail first** (on `backdrivingLifter()` with `setBrakeTable()`, and `Rig::watchSafe`
on): a 33-tick step down (942 → 909) moves and ends within ±5 of its target; steps of 20, 50, 100
and 300 ticks each way end within ±5; existing long-seek tests keep their tolerances; a clamped seek
with the head turned comes to rest at or above the floor with no table and with the table (the
review's probe rested at 572 and 573 against 575). Then all existing tests.

**Bench (Part A), staged so nothing can reach the frame even if the change misbehaves:** the head
home for every descent. Record target against resting height for the Operate presets (25/50/75 %)
and a few typed short steps (for example `:PP80` then `:PP77`); a minute of random mode; one E-STOP
mid-descent with the head home (braking after a latch is unchanged, a sanity check). The clamp margin
only like this: `#PSAFESPIN1100` for the test, head turned 90° at the top, then `:PP85` (target 966,
above the measured 878, so a failed clamp would still stop clear); expect rest at or a few ticks above
1100; then **Home the head** and `#PSAFESPIN878`. Never a turned head near 878.

## Phase 5 scope (plan §5, Appendix C.2–C.4)

1. **Vendored WCBClient** in `UppityFirmware/libs/` (or pinned in `sketch.yaml`): one tag for this
   board. The library index has 1.13.2, Roam-A-Dome ALT vendors 1.13.0, Sabé builds against
   1.9.10. Check which the fleet's WCB firmware expects before choosing.
2. **`Mesh.cpp`** (Arduino glue): `WCB_Client(oct2, oct3, password, quantity, deviceId, cb,
   statusCb)`, `setMeshChannel`, `setChecksum`, `setIdentity(...)`, `enableSpecialPeer(20)`,
   `WiFi.setSleep(false)`; refuses to start without a password. **Off by default.** Inbound `:P`/
   `#P` lines go to the command queue as source `Mesh`; `&ESTOP` trips the latch; `&ESTOP,RESET`
   releases it; `&DISABLE`, `&STATUS`, `&HELLO` per C.2; everything else silent.
3. **Outbound `&PERI`** per C.2: `HB,<fw>,<uptimeS>,<state>,<height%>,<rotDeg>,<homed>` broadcast
   every `hbPeriodS` (10 s default, 5–300); `POS` unicast to 20 on change, at most 1 Hz; `STATE` on
   transitions; `FAULT` ensured unicast with broadcast fallback; `ACK/NAK` only for `&` verbs
   addressed to it. Broadcast payloads ≤ 187 characters.
4. **Dedup across serial and mesh**: the same command arriving on the command UART and over the
   mesh within 750 ms executes once (Sabé can reach the periscope both ways during a transition).
5. **WiFi interplay**: with mesh on, the access point sits on the mesh channel and the home-network
   join is refused (the library takes the station side and rewrites the MAC). Todd then reaches
   the page only at `http://192.168.4.1` on `R2Uppity-FB3F`. Say this plainly in the page, the
   console and the bench doc before he turns mesh on.
6. **Settings**: `meshEnabled`, `oct2`, `oct3`, `meshPass` (write-only, never printed), `wcbQty`,
   `deviceId`, `meshChannel`, `checksum`, `hbPeriodS`: append-only, schema bump, `#PCONFIG`
   replayable (the password is never in it), and a **Setup → Mesh** page.
7. **Diagnostics**: mesh status, neighbours seen, last heartbeat sent and received.
8. **DroidNet Command Library**: regenerate `libraries/boards/r2uppityspinner-alt.json` for 4.0
   against the current schema (`category`, not `group`; the copy in this repo's root is stale).
   The PR to Travis is a public action: Todd approves it separately.

Verification per the plan: on the bench with Sabé and the WCB fleet, `:PP100` over the mesh,
`&PERI,HB` visible in the DroidNet console, `?STOP` latches, `&ESTOP,RESET` releases.

## Ask Todd before the code touches the mesh

- Which **WCB firmware** each board in his fleet runs (6.2.0 or newer is needed, or the periscope's
  messages are invisible to it), and how many WCBs there are (`wcbQty`).
- The mesh's **MAC octets** (`oct2`, `oct3`) and **channel**, and the **mesh password**. He types
  the password into the page himself; it is never echoed, logged or written down.
- The periscope's **device ID** (WCBs are 1…quantity, 20 is the special slot Sabé uses).
- How Sabé reaches the periscope today (the plan says through WCB 3, serial 4: `;W3;S4`) and
  whether that path stays during the transition.
- Sabé-side changes (show `&PERI` on its dashboard, broadcast `&ESTOP,RESET` on its own reset,
  route `:P` to the periscope's device ID) are a separate repository and a separate approval.

## Todd's workflow (he is not a coder; every instruction is a complete copy-paste command)

Work lands on a session worktree branch; Todd fast-forwards `v4-rewrite` in the main checkout
himself (Claude's merge attempts are blocked by the tool policy):
```
git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only <session branch>
```
Then he builds the install file and uploads it from the page (**Setup → Firmware**):
```
cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
```
The monitor, in its own Terminal window (a new log file name per bench run):
```
arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase5-log.txt
```
The monitor echoes typed lines, so a typed password lands in the log file; never ask him to type
one there.

## Open items carried forward

- Phase 4, optional: Part E step 32 (an upload refused while a motor runs), Part F (commissioning
  from a factory reset).
- Not yet tested from earlier phases: two phones on the page at once, the command UART (Sabé bus,
  9600), the trigger inputs.
- **Short seeks stop short**: now Part A of this phase (above).
- From the code review, still open: D6 (`LIMIT_MISMATCH` for a wrong `#PTRAVEL`), D7 (jog press
  ids), D8 (`#PCONFIG` tpr confidence). D2 on the droid (2026-10-04): Stop ended a running rotary
  test; the queued-move half was not exercised (the 90° press never reached the board), so it rests
  on its host test.
- Backlog: nudges land a few ticks short going up (+9 for +12; see Part A item 4). A device PIN for
  mutating web routes. The `TICK` lines during an upload could be quieter.
- Keep an eye on the rotary drive play (coupler set screw / encoder disc); homing copes with it.

## Paste this to start the next session

    Continuing the Uppity Spinner ALT v4 rewrite. Phases 0-4 and the October code review are
    bench-verified on my periscope. Read CLAUDE.md first, then docs/PHASE5-BRIEF.md, then
    docs/REWRITE-PLAN.md section 1.1, the phase table in section 5 and Appendix C, the summary
    and "Your decisions" sections of docs/CODE-REVIEW-2026-10.md, then memory project_v4_rewrite
    and project_todd_periscope_hardware. Start Phase 5 from v4-rewrite. Do Part A first: D5, the
    seek braking change, exactly as the brief describes it, with host tests that fail before and
    pass after, and bench it as Part A of docs/BENCH-PHASE5.md (the head home for every descent,
    the clamp test only with #PSAFESPIN1100 and a :PP85 target, then #PSAFESPIN878 back). Then the
    mesh and DroidNet with the &PERI heartbeat tag; ask me the WCB fleet questions in the brief
    before writing mesh code that depends on them. Mesh stays off by default, and mesh commands
    are unattended: they never get the rescue override or the commissioning unlock. Keep the
    definition of done from CLAUDE.md (host tests pass, ESP32 compile clean, size line recorded),
    write docs/BENCH-PHASE5.md for me in the same voice as the earlier bench docs with complete
    copy-paste commands (real paths, port /dev/cu.wchusbserial110, installs through the page's
    Setup -> Firmware upload), commit on the session branch and give me the merge line, and never
    script a move that depends on a clearance you have not confirmed on my hardware. Nothing
    public (pull requests, posts) without my approval each time. I am not a coder.
