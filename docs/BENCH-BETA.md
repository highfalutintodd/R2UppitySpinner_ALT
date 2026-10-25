# Bench procedure — before 4.0.0-beta.1 goes to testers

For Todd. Everything in the beta has run on your droid except the pieces below, and those are the
ones testers will reach first: the command serial port driven by another board, the same command
arriving over the wire and the mesh, a mesh E-STOP while the lifter moves, Sabé's own E-STOP, the
two fixes from 2026-10-24's bench, the new default-password warning, a fresh install from the
release files, and a long run. **The dome is on for all of it** (WCB3 needs power, and safe-spin
878 was measured with the dome on).

**Rule for every step:** the head is home for every descent, and the head turns only at the top.
Nothing is held: every move is a fixed target. Nothing here depends on a clearance that has not
been measured on your droid.

The mesh is on (as left on 2026-10-24), so the board is **not** on WordNet: its page is at
`http://192.168.4.1` on `R2Uppity-1390`. Keep the laptop on WordNet (so it can reach me) and use
the iPhone for the page, except in step 3, where the laptop joins `R2Uppity-1390` to upload.

## Part 1 — install the beta, from the release files

1. In the Terminal app, bring the work into your checkout:
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/phase5-mesh
   ```
   Success looks like `Fast-forward` and a list of files.
2. Make the release files (the same ones testers will download). It builds the firmware, so it
   takes a few minutes:
   ```
   "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3/tools/release.sh"
   ```
   Success: `Layout checked: bootloader 0x1000, partitions 0x8000, settings 0x9000, firmware
   0x10000 …` and `Release files for 4.0.0-beta.1 are in: /Users/toddword/Desktop/uppity-spinner-alt-4.0.0-beta.1`.
   (If it says the folder already exists, drag that folder to the Trash and run it again.)
3. In a second Terminal window, the monitor (leave it running for the whole bench):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee -a ~/Desktop/uppity-bench-beta-log.txt
   ```
   Opening it restarts the board, as usual.
4. Upload the update file from the page, as a tester would. Join the **laptop** to `R2Uppity-1390`
   (password `Astromech`), open `http://192.168.4.1`, then **Setup → Firmware → Choose File**: in
   the `uppity-spinner-alt-4.0.0-beta.1` folder on your Desktop pick
   `uppity-spinner-alt-4.0.0-beta.1-update.bin`, then **Upload**. *Success:* `Written. The board is
   restarting…`. Put the laptop back on WordNet.
5. **Boot lines** (I read them from the log): `Uppity Spinner ALT v4.0.0-beta.1  build …` (no
   "phase" any more), your usual `CALIBRATION` and `CONFIG` numbers, `WIFI AP "R2Uppity-1390"`,
   then the new line `WIFI WARNING: the board's own network uses the published default password…`,
   and `MESH=joined as id 5 …`. On the iPhone page, a warning banner about the default password with a
   **WiFi settings** button. Leave it for now (step 14).

## Part 2 — the wire, the mesh and E-STOP, with the dome on

6. **All three WCBs.** In the monitor:
   ```
   #PSTATUS
   ```
   *Success:* the `MESH=` line says `wcbs=3/3 online controller=online`.
7. **Head home, mast at the bottom.** On the iPhone page: **Operate → Down** (it homes the head
   first if needed). Wait for it to stop.
8. **The command wire on its own: a question.** On WCB1's console:
   ```
   ;W2;S2#PSTATE
   ```
   (WCB2 writes `#PSTATE` out of its serial port 2, into the periscope's command port. The wire is
   on WCB2 port 2, found 2026-10-24; not WCB3 port 4, and not WCB2 port 4, the inner rim.) *Success:*
   the periscope's `STATE=…` lines in the monitor. This is the first time v4's command port has
   been driven by another board. If nothing arrives, stop here and tell me.
9. **The same move over the wire and the mesh at once (step 19 of Phase 5).** On WCB1's console,
   one line:
   ```
   ;W2;S2:PP100^;W5:PP100
   ```
   *Success:* the mast rises to the top **once** (one `SEEK START`). If the wire's copy came first,
   the monitor's `MESH :PP100 from 1` line ends `(a duplicate of the same line on the serial bus:
   ran once)`; if the mesh's came first, the wire's copy is dropped quietly. Then, in the monitor:
   ```
   #PSTATUS
   ```
   and its `SERIAL=` line shows `duplicates=` one higher than before (it was 2 after Comlink's
   two broadcasts on 2026-10-24). Up is always allowed, so even two runs
   would be harmless.
10. **Down over the wire.** On WCB1's console:
    ```
    ;W2;S2:PH
    ```
    *Success:* the head homes if it needs to, and the mast lowers to the bottom.
11. **A mesh E-STOP while the lifter moves.** The head is home. Raise the mast first, from the
    monitor:
    ```
    :PP100
    ```
    When it has stopped at the top, type this **one line** on WCB1's console. It sends the descent,
    and 0.4 s later the E-STOP, both to the periscope:
    ```
    ;W5:PP0^;T400,;W5&ESTOP
    ```
    *Success:* `MESH :PP0 from 1`, `SEEK START`, then about 0.4 s later `MESH &ESTOP from 1:
    E-STOP latched`, `ESTOP braked to a stop: lifter -NN …` with NN about 50–60 (as from the page),
    and the mast stopped part-way down. If the timing comes out differently (the E-STOP lands at
    once, or the mast reaches the bottom first), that is harmless with the head home; the log tells
    me which. Then release it from the page: **Reset**.
12. **Sabé's own E-STOP.** Mast at the top (`:PP100` in the monitor if it is not), then start a
    slow spin at the top:
    ```
    :PR40
    ```
    Press Sabé's E-STOP (however you normally do). *Success:* the spin stops at once and the
    monitor shows `ESTOP LATCHED` (the source number says whether it came over the mesh or the wire).
    Release on Sabé if it sends a release; otherwise on the page: **Reset**. Then **Operate → Home**.
13. **The &STATUS fix.** On WCB1's console, three times, a few seconds apart:
    ```
    ;W5&STATUS
    ```
    *Success:* each time, WCB1 shows all three: `&PERI,HB,…`, `&PERI,STATE,IDLE` and
    `&PERI,ACK,STATUS`. Paste me that part of WCB1's console.

## Part 3 — a long run

14. **Random medium for 30 minutes.** On the page: **Operate → Random medium**. Let it run; watch
    now and then. Then **Stop**, and in the monitor:
    ```
    #PSTATUS
    ```
    *Success:* no `FAULT` lines in the log, every turn at the top or above 878, every descent below
    878 after a home, and in `#PSTATUS` the `MESH=` line still `wcbs=3/3`, `txFail=0`.

## Part 4 — a fresh install from the full image, on the droid (what every tester does first)

This erases everything stored on the board: your settings, the calibration, the mesh settings,
WordNet's details, and even v3's old values (so nothing is imported). The board then boots as a
brand-new one would: LOCKED, on its own network with the default password. Commissioning measures
everything again with the dome on, as on 2026-10-04, and Part 5 puts back what commissioning does
not measure. About an hour in all. Nothing here moves until commissioning step 3, and every
commissioning step says what it will do before it moves.

**Your measured numbers, for comparison and as the guard in step 6** (2026-10-04, dome on):
travel 1137 between the switches, rest 37, rotary about 1185 ticks per turn, safe-spin 878: the
turned head was stopped at **844**, plus a 34-tick margin. That day it had been down to about 800
without touching, but **844 is the lowest a turned head goes in this procedure.**

15. **Save what is on the board now.** Head home, mast at the bottom (**Operate → Down** on the
    page). In the monitor, one at a time (the answers land in the log file, which is your backup):
    ```
    #PCONFIG
    ```
    ```
    #PCAL
    ```
16. **Close the monitor:** click its Terminal window and press **Control-C**. The USB port must be
    free for the flash.
17. **Flash the full image.** In **Chrome** (or Edge; Safari cannot do this), open
    [https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/). Set
    Baudrate to `460800`, press **Connect**, and pick the board's port in the list (it mentions
    `wchusbserial` or `USB Serial`). Under **Program**: Flash Address `0x0`, and for the file, in the
    `uppity-spinner-alt-4.0.0-beta.1` folder on your Desktop,
    `uppity-spinner-alt-4.0.0-beta.1-full.bin`. Press **Program** and wait for 100 %. Then press
    **Disconnect** and close the tab. (Do not press **Erase Flash**; it is not needed.)
    If the page will not connect, use this one line in the Terminal app instead; it does the same:
    ```
    ~/Library/Arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool --chip esp32 --port /dev/cu.wchusbserial110 --baud 460800 write-flash 0x0 ~/Desktop/uppity-spinner-alt-4.0.0-beta.1/uppity-spinner-alt-4.0.0-beta.1-full.bin
    ```
    *Success:* it writes 4 MB, verifies, and resets the board (`Hash of data verified`).
18. **The monitor again**, with a new log file:
    ```
    arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee -a ~/Desktop/uppity-bench-beta-fresh-log.txt
    ```
    *Success (the boot):* `SETTINGS=none (first boot of v4)`, `V3 import: no v3 settings in flash
    (nothing to take)`, a `CONFIG` line with `commissioned=0`, `WIFI AP "R2Uppity-1390" password "Astromech" (the
    default…)`, the `WIFI WARNING` line, and `MESH=off`. Nothing moves.
19. **Join the iPhone** to `R2Uppity-1390` with the password `Astromech` (the phone remembers your
    new one from earlier, if you set it; choose "Forget This Network" and join again) and open
    `http://192.168.4.1`. *Success:* the LOCKED banner and the default-password banner. **Leave the
    password for now** (Part 5).
20. **Setup → Commissioning → Start.** The steps, with what is different on a blank board:
    - **1 · Profile:** check that **Pololu 4751 (19:1)** is selected (it is the default). Tick the
      boxes, **Next step**.
    - **2 · Wiring check:** nothing moves. **Press Flip so "Runs backwards" reads yes** (a blank
      board does not know your lifter is inverted). With the mast down the bottom lamp is lit. Tick
      the box, **Next step**. (If you forget the flip, step 3 pushes on the bottom stop for under a
      second, stops, and tells you to flip it; flip it and run step 3 again.)
    - **3 · Lift test:** **Run the lift test**: a slow creep up to the top switch.
    - **4 · Rotary:** **Measure the rotary**: three turns at the top, then home. Expect about 1185.
    - **5 · Calibration:** **Calibrate the lifter**: about six minutes. Expect travel about 1137
      and `rest` about 37.
    - **6 · Safe-spin height:** the one that needs your eyes, exactly as on 2026-10-04.
      **a.** **Raise and turn the head** (top, 90°). **b.** **Override on (60 s)**, and press it again
      whenever the countdown gets low. **c.** **Lower 3 %** (about 35 ticks a press) until the
      Height line reads about 900, then **Lower 1 %** (about 12) only. After every press, look at
      the lowest part of the head against the dome edge **and every part of the frame it would
      pass over if it turned**. **Stop at about 850, and never below 844**, or sooner if anything
      looks close. **d.** **Use this height…**, then **Tap again: store it**. Expect about 878-885.
      **e.** **Raise and home the head**, then **End the override**.
    - **7 · Acceptance:** **Run the acceptance** (home, 0 %, 50 %, 100 %, home, one gentle random
      action), then **Finish: mark commissioned**. *Success:* `COMMISSIONED=2`, seven green ticks.

    If a step fails, its box turns red and says why; tell me before running it again unless it is
    the flip in step 3.

## Part 5 — put back what commissioning does not measure, and your own password

21. **Your tuned numbers.** In the monitor, one at a time (each answers `(saved)`):
    ```
    #PMINUP50
    ```
    ```
    #PMINDOWN50
    ```
    ```
    #PRANDOMLIGHT7
    ```
    (Your v3 floor of 50 % was what every bench since Phase 2 ran on; the calibration alone would
    use about 25 % up and 15 % down.)
22. **The mesh**, on the iPhone page: **Setup → Mesh**, the same eight fields as Phase 5 step 8:
    octets `3C:4E`, the mesh password (on the page only), WCBs `3`, mesh number `5`, channel `1`,
    checksum On, heartbeat `10`, Join the mesh On.
23. **Your own password for the board's network**: **Setup → WiFi**, the board's own network
    password, 8 to 32 characters (write it down somewhere safe), **Save**. (Optional: your WordNet
    name and password too, for whenever the mesh is off.) **Never type a password into the
    monitor:** it would land in the log file.
24. **Restart to apply** (tap twice). Rejoin the iPhone to `R2Uppity-1390` with the **new** password
    and open `http://192.168.4.1`. *Success:* no banners; in the monitor `password <yours>`, no
    `WIFI WARNING`, `MESH=joined as id 5 …`, and then `MESH WCB1 online`, `WCB2`, `WCB3`,
    `controller 20`. If you cannot get back in, type these two lines into the monitor to put the
    published default back (it is no secret), and try again:
    ```
    #PAPPASS=Astromech
    ```
    ```
    #PRESTART
    ```
25. **Compare.** In the monitor:
    ```
    #PCONFIG
    ```
    I compare it with the one from step 15.

## Send back

The two log files from your Desktop (`uppity-bench-beta-log.txt` and
`uppity-bench-beta-fresh-log.txt`), WCB1's console for steps 9, 11 and 13, and anything that looked
or sounded different.

## Results 2026-10-24, Parts 1-3 (build 6bf45839; the dome on from step 6)

Parts 1 to 3 passed. Parts 4 and 5 have not run yet. The log is `uppity-bench-beta-log.txt` on
Todd's Desktop; WCB1's console (debug on) was pasted into the session. The page ran on Todd's
iPhone.

**Part 1, install.** `tools/release.sh` made the folder on the Desktop (all six checksums good).
`…-update.bin` went up from the page over `R2Uppity-1390`, as a tester would do it. Boot:
`Uppity Spinner ALT v4.0.0-beta.1  build 6bf45839`, settings and calibration unchanged (travel
1137, rest 37, safe-spin 878, 1185.3 ticks/rev, commissioned), the new `WIFI WARNING` line and the
page banner, `MESH=joined as id 5`.

**Part 2, the wire, the mesh and E-STOP.**
- **6:** `wcbs=3/3 online controller=online` once the dome was powered; `txFail=0 txDropped=0`.
- **8, the wire is on WCB2 serial port 2.** `;W3;S4…` (from old notes) and `;W2;S4…` reached
  nothing: WCB2's port 4 is the inner rim, where the hologram lifter's own Uppity Spinner board
  lives. Todd traced the periscope's cable to **WCB2 port 2**. `;W2;S2#PSTATE` then answered with
  the full state: the first command deliberately sent to v4 over the wire. The doc's steps 8 to 10
  now use `;W2;S2`.
- **Broadcasts reach it both ways.** Comlink's `:PL7:PP100` and `:PH` (typed into WCB1, which
  broadcasts them) arrived over the mesh (`MESH :PL7:PP100 from 1`) and over the wire (WCB2 passes
  broadcasts out port 2). Each ran once; `#PSTATUS`: `duplicates=2`.
- **9:** `;W2;S2:PP100^;W5:PP100`: the wire's copy ran, the mesh's was dropped (`MESH :PP100 from
  1 (a duplicate of the same line on the serial bus: ran once)`), `duplicates=3`. With Comlink's
  mesh-first pair, both orders have now been seen on the droid: step 19 of Phase 5 passes.
- **10:** `;W2;S2:PH`: a safe lower over the wire.
- **11:** `;W5:PP0^;T400,;W5&ESTOP`: the E-STOP latched 394 ms into the descent (`ESTOP LATCHED
  (source 4)`), `lifter -56 ticks … drivers off after 211 ms`, the same as from the page (−59/−62).
- **12:** Sabé's E-STOP during a `:PR40` spin at the top arrived over the mesh as `?STOP` from 20:
  latched (source 4), `rotary -12 … drivers off after 198 ms`. Sabé sends no release that v4
  understands yet (the Sabé-side `&ESTOP,RESET`), so `#PRESUME` released it. Roam-A-Dome latched
  as well and needs its own release.
- **13, the `&STATUS` fix:** three sends, each answered with all three lines (`&PERI,HB`,
  `&PERI,STATE,IDLE`, `&PERI,ACK,STATUS`). Before the fix the middle one was lost.
- The iPhone could not rejoin `R2Uppity-1390` at one point. `#PRESUME`, `:PA0` and `#PRESTART`
  from the monitor brought the board back cleanly (at the top, head home, all boards online).

**Part 3, the long run.** Random medium for about 22 minutes (stopped early, with plenty logged):
125 descents into the dome, each right after a home that parked on the switch with rest contact;
202 turns, spins and homes, every one at or above 878; 265 seeks to a height, each at rest −8 to +9
from its target; no fault, refusal or clamp; `#PSTATUS` afterwards `wcbs=3/3`, `txFail=0`,
`txDropped=0`.

**One light scrape, about 8 minutes in:** on a descent the head was "ever so slightly off
center" and the front lens brushed the dome's pie-panel opening. Todd nudged the head back while
it moved; it did not happen again in the remaining ~14 minutes. What the firmware saw and does:
- Every descent followed a confirmed home; no rule was broken.
- The home switch stays closed over 35-40 ticks (about 11-12°), and the board always parks at the
  same edge of it, approached from the same side.
- Both drivers share one enable line, so the rotary motor is braked whenever the lifter moves: the
  head can shift only within its gearbox's own slop, which Todd puts at 2-3° (no looseness in the
  coupler or screws; it has always been there).
- The opening leaves very little room for the lens, so the remaining lever is where home sits in
  it: `#PHOMEOFFSET` (about 3.3 ticks per degree on this rotary), chosen by looking at the gap on
  each side of the lens with the periscope down, then checked at the top before any descent. Not
  done yet; not a blocker for the beta. The README now has this under Troubleshooting.

**Not run yet:** Part 4 (the fresh install from the full image, then commissioning from scratch)
and Part 5 (putting back the tuned numbers and the mesh, and the board's own WiFi password). Until
Part 5 the board runs the mesh with the published default access-point password.

## Results 2026-10-24 (evening), Parts 4-5: a fresh install on the droid (build 6bf45839, dome on)

Both passed. The log is `uppity-bench-beta-fresh-log.txt` on Todd's Desktop; the pre-erase
backup is `uppity-backup-before-erase-2026-10-24.txt` next to it.

- **15, backup:** `#PCONFIG` and `#PCAL` saved before the erase (and copied to their own file,
  because the monitor's `tee` had already emptied `uppity-bench-beta-log.txt` once that evening by
  reusing its name; Parts 1-3 had been written up already).
- **16-17, the full image:** the release's `…-full.bin` flashed at `0x0` and verified.
- **18-19, a blank board:** `inverted=0 travel=1350 (profile guess) safespin=0 tpr=0.0
  commissioned=0`, no calibration, `MESH=off`, `password "Astromech" (the default…)` with the
  `WIFI WARNING` line; the iPhone showed the LOCKED and default-password banners.
- **20, commissioning from scratch, all seven steps:**
  - 1-2: profile 19:1 kept; **Flip** set `#PINVERT=1 (saved) (re-referenced from the limit switches)`.
  - 3: creep up to the top switch (1151 ticks counted from the resting spot).
  - 4: **1185.0 ticks/rev**, spread 0.2 %, contact arc 56, confident; home found.
  - 5: **travel 1138**, spread 0.0 %, lowest power **25 % up / 15 % down**, **rest 37**.
  - 6: head turned 90° at the top, override on; eight **Lower 3 %** (29-32 each, asked 34) to about
    900, six **Lower 1 %** (10-11 each) to about 836 at the lowest, two **Raise 1 %** (+8 each) to
    about 852, the head clearly clear throughout. That lowest point is about 8 under the 844 the
    procedure gave as a floor, and some 36 above the ~800 the turned head reached without touching
    on 2026-10-04. Stored **`SAFESPIN=886`** (measured 852 + 34 margin), 8 above the old 878.
  - 7: home, 0 %, 50 % (rest 563 for 569), 100 %, home, one gentle random turn at the top;
    `COMMISSIONED=2`, LOCKED → IDLE.
- **21-24, put back:** `#PMINUP=50`, `#PMINDOWN=50`, `#PRANDOMLIGHT=7`, the eight mesh keys, WordNet
  (stored for when the mesh is off) and the board's own password, all from the page except the
  first three; one restart: `password <yours>`, no `WIFI WARNING`, `MESH=joined as id 5`, all four
  boards online, no banners on the page.
- **25, compare:** `#PCONFIG` against the backup differs only in the measured numbers (safe-spin
  878 → 886, ticks/rev 1185.3 → 1185.0, travel 1137 → 1138) and `APPASS default` → `custom`.

**The pre-beta bench is complete.** What it leaves for the record: the light lens scrape in Part 3
(centring home with `#PHOMEOFFSET` is optional, not done), and that a turned head was lowered a few
ticks past the procedure's floor in step 6 without contact. For later bench docs: start the monitor
with `tee -a`, so a reused file name appends instead of emptying the file.
