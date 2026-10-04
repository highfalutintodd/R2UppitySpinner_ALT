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
dome on. Latest commit on the session branch `claude/uppity-spinner-phase-4-1e5680`: the one
that added this brief (Todd fast-forwards `v4-rewrite` to it). Build on the droid: `9743ad66`.
Host tests: 161. Flash: 1,176,647 bytes of 1,966,080 (59 %).

Todd's numbers (all stored on the board; nothing to re-send after a flash): 19:1 profile,
inverted, travel 1137 between the switch closings (calibrated, `#PTRAVEL0`), the mast rests up
to 37 ticks below the bottom switch's closing point (`rest=37`, used at power-up), **safe-spin 878,
measured with the dome on** (2026-10-04; it sits ~15 ticks on the safe side of what commissioning
intended, leave it), 1185.0 ticks/rev, home park 1, wiggle 2, `COMMISSIONED=2`,
`#PMINUP/#PMINDOWN` 50 % from the v3 import, `#PRANDOMLIGHT=7`. House network `WordNet` (the
password is stored on the board and is never printed or written down); the board's house address
is DHCP and has been `192.168.86.69` lately (the `WIFI joined` boot line shows it).

**Heartbeat tag: `&PERI`** (Todd's decision, 2026-10-04).

## Lessons from Phase 4 that shape Phase 5

- **Safety paths the simulator cannot see go untested.** E-STOP disabled the drivers at once and
  the mast free-wheeled down, because the simulator modelled the screw as self-locking (it now
  has a back-drive model). A mesh E-STOP must land on the same latch as every other source
  (`EstopLatch::trip`, from any context) and nothing else; test it on the simulator and bench it.
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
- Backlog: nudges land a few ticks short going up (+9 for +12): the braking lead uses the median
  of the calibration table, and slow upward creeps stop quicker than that; a speed-matched lead
  from the same table would fix it. A device PIN for mutating web routes. The `TICK` lines during
  an upload could be quieter.
- Keep an eye on the rotary drive play (coupler set screw / encoder disc); homing copes with it.

## Paste this to start the next session

    Continuing the Uppity Spinner ALT v4 rewrite. Phases 0-4 are bench-verified on my periscope.
    Read CLAUDE.md first, then docs/PHASE5-BRIEF.md, then docs/REWRITE-PLAN.md section 1.1, the
    phase table in section 5 and Appendix C, then memory project_v4_rewrite and
    project_todd_periscope_hardware. Start Phase 5 (mesh and DroidNet) with the &PERI heartbeat
    tag; ask me the WCB fleet questions in the brief before writing mesh code that depends on
    them. Mesh stays off by default. Keep the definition of done from CLAUDE.md, write
    docs/BENCH-PHASE5.md for me in the same voice as the earlier bench docs with complete
    copy-paste commands (real paths, port /dev/cu.wchusbserial110, installs through the page's
    Setup -> Firmware upload), commit on the session branch and give me the merge line, and never
    script a move that depends on a clearance you have not confirmed on my hardware. Nothing
    public (pull requests, posts) without my approval each time. I am not a coder.
