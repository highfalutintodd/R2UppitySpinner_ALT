# Bench procedure — Phase 4 (the full web page)

For Todd. Phase 4 is the rest of the web page: four pages instead of one (Operate, Setup,
Diagnostics, Rescue), settings you can change from the phone, a firmware upload button, the
rescue tools (a 60-second safety override and hold-to-jog), and commissioning, the guided setup
that measures a periscope step by step, including a safe-spin height you measure by eye. The
motion core, the E-STOP and everything from Phases 1–3 behave as before, with the three small
safety changes listed under "What is different".

Everything here was checked on the Mac: 139 host tests pass (23 new), the firmware compiles
clean with every warning on (1,173,871 bytes, 59 %), and every page was driven in a browser
against the simulated periscope in `webui/dev-server.py`, at phone and desktop width, in both
themes. None of it has run on the periscope yet; that is this procedure.

**Rule for every step below:** nothing here turns the head anywhere you have not already seen it
turn safely (at the top, and at 80 % = 920 ticks, 2026-09-20), and nothing lowers a turned head
below 1000 ticks, except in commissioning step 6, where you do the lowering yourself, a small
step at a time, watching. A turned head is only ever lowered by a fixed-size step (`#PNUDGE`),
never by holding a jog button: if the phone's "let go" were lost to the radio, a held jog keeps
going for up to 2 seconds.

## Part A — Merge, flash, back up

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/uppity-spinner-phase-4-1e5680
   ```
   Success looks like `Fast-forward` and a list of files.
2. With the periscope idle and the laptop on the house WiFi, build and send it over WiFi, one
   line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   ```
   python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.4/tools/espota.py -r -i 192.168.86.69 -p 3232 -a Astromech -f ~/Desktop/uppity-build/UppityFirmware.ino.bin
   ```
   The compile ends with `Sketch uses 1173871 bytes (59%)` (a few bytes either way is fine);
   the upload with `Result: OK`. The house network can give the board a new address: the
   monitor's boot line `WIFI joined "WordNet": … (192.168.86.xx, …)` shows the current one (it
   was 192.168.86.69 on 2026-10-03), and `#PSTATUS` prints it after `home=`. If OTA says
   `Host … Not Found`, use that address, or flash over USB (monitor closed, same folder):
   ```
   arduino-cli upload --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
   ```
3. The monitor, in its own Terminal window:
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-log.txt
   ```
4. Boot lines to look for: `Uppity Spinner ALT v4.0.0-dev  phase 4  build ...`, then once only
   `SETTINGS=migrated from schema 3 (224 bytes) to 4` followed by `PERSIST=saved settings` (the
   settings gained one byte for the commissioning step). The `CONFIG ...` line must still show
   your numbers: `motor=1`, `inverted=1`, `travel=1150 (#PTRAVEL)`, `safespin=904`,
   `tpr=1185.5`, `commissioned=1`.
5. On the phone (house WiFi), open `http://` followed by the address from the boot line (for
   example `http://192.168.86.69`), or `http://uppity.local`. The page now has four tabs along the
   bottom (a column on the left on a laptop). Go to **Diagnostics → Configuration → Get the
   settings text**, then **Download** (or Copy and paste it into Notes). This is your backup: every
   setting as a line you can send back later. Do this before anything else.

## What is different, in one breath

- **Four pages.** Operate is the Phase 3 page without the console. The console (the log and the
  command box) moved to Diagnostics. Setup holds commissioning and every setting. Rescue holds the
  override, the jogs, "set home here" and clear fault. The red E-STOP is on every page, unchanged.
- **Banners at the top** say when the E-STOP is latched, a fault is showing, the override is on
  (with its countdown), the board is LOCKED, commissioning is active, or an update is running.
- **New serial commands**, all also on the page: `#POVERRIDE` / `#POVERRIDE0` (the 60 s safety
  override; only accepted from the USB console or the page, never from the Marcduino bus),
  `#PJOGL+` / `#PJOGL-` / `#PJOGR+` / `#PJOGR-` (a 2-second jog), `#PNUDGE-12` (lower the mast 12
  ticks at the slow power; + raises), `#PSETHOME` (the head is lined up: that is home now),
  `#PCOMMISSION…` and `#PSAFESPINHERE` (commissioning).
- **Three safety changes**, all narrowing what is allowed:
  1. A head at the top switch is never treated as "inside the dome opening" (before, with no
     safe-spin height set, a turned head at exactly the travel count could have been let down).
  2. Once the override has let the head come down unaligned, or turned it below the safe-spin
     height, descents are refused until the head is home again. The log says
     `ALIGNMENT suspect: …` when that starts, `ALIGNMENT above the safe-spin height again …` if
     the mast rises past that height before the head has homed, and `ALIGNMENT ok again` when a
     home clears it below that height.
  3. A creep (commissioning, calibration, jog) stops at once if the switch it is moving away from
     closes again: that only happens when the mechanism runs backwards. The log says
     `WRONG_LIMIT` and a `HINT=` line suggests `#PINVERT`.
- **Jog power.** Jogs and nudges use the slowest continuous power the calibration measured moving
  your lifter (25 % up, 15 % down on yours), not the 50 % floor your seeks use, so they creep.

## Part B — The page tour (nothing moves in B)

6. **Operate**: same buttons as Phase 3. Tap **Up** then **Down** once to see it still drives.
7. **Diagnostics → Live sensors**: the switch lamps (top, bottom, home), the drivers, the counts,
   the rotary's trust level with where it came from (`SWITCH`, `EDGE` or `MANUAL`), the tick
   stats, the WiFi and E-STOP counters. **Log**: type `HOME` in the filter box and only home lines
   show; untick **hide TICK** to see the health lines; **Download** saves the log to the phone.
   Type `#PSTATE` in the command box and **Send**; the answer appears in the log.
8. **Setup → Tuning**: change **Settle wiggle** from 2 to 3 and **Save**. Under the field it says
   `Saved` (that is the board's own `#PWIGGLE=3 (saved)` line coming back). Set it back to 2.
   Then in **Safe-spin height** type 900 and Save: it does not save on the first tap, it asks you
   to tap again (lowering it lets the head turn lower). Do **not** tap again; type 904 back or
   leave the page.
9. **Setup → Motor**: the calibration box shows your calibration (travel measured 1179, lowest
   power 25 / 15 %) and that `#PMINUP/#PMINDOWN` (50 / 50) override it for seeks. Nothing to change.
10. **Setup → Theme**: **Dark**, then look at each of the four pages; everything should be
    readable and the tab you are on stands out. Switch back to **Paper** (or keep dark; the board
    remembers either).

## Part C — Rescue (the override honoured for the full 60 s)

These steps borrow a higher safe-spin height, **1100 ticks**, so that everything the override
lets through still happens above 1000 ticks, where you have watched the head turn freely.

11. On the monitor: `#PSAFESPIN1100`. Answer: `#PSAFESPIN=1100 (saved)`.
12. **Operate → Up**, wait for 100 %, then **90°**. The head turns at the top (allowed: the top
    switch is made).
13. **Rescue → Lifter jog**, power **Slow**. Hold **▼ Down**. The mast creeps down and stops by
    itself at 1100 (`CREEP CLAMPED ROTARY_NOT_HOME`): the head is not home, so without the override
    it may not go lower. Let go, and **do not hold Down again** to check that it stays: on
    2026-10-03 the next press was let through and the turned head hit the lifter frame (fixed in
    the next build; see Results at the end). Whether it stays is checked with a fixed-size step in
    the re-test, where a failure could only move 20 ticks.
14. **Safety override: press and hold** the button. It counts `Keep holding… 2`, `… 1`, then turns
    on: an amber banner `SAFETY OVERRIDE ON: 60 s left` counting down, and on the monitor
    `OVERRIDE=on (60 s; …)` and `OVERRIDE on for 60s: …`. **Note the time.** A short press (let go
    before 3 s) must not turn it on; try that first if you like.
15. While it counts down, type `#PNUDGE-50` on the monitor. Answer: `NUDGE=-50 ticks at 15% …`;
    the mast goes 50 ticks below 1100 and stops by itself (about 1050; the Height line in the Now
    box shows the ticks). Only the override allows that with the head turned; the monitor shows
    `ALIGNMENT suspect …`.
16. Still in the 60 s, set the rotary jog power to **Normal** (your rotary's 40 %) and hold
    **◀ Turn** for a second: the head turns a little, at about 1050, well above the 920 where you
    have seen it turn freely. Below 1100 this is only allowed with the override on. Let go. (Then
    try one hold on **Slow**, 26 %, and tell me whether it moves at all: your drive may need more.)
17. Do nothing until the banner reaches 0. The monitor prints `OVERRIDE off` about 60 s after
    step 14 (tell me how many seconds it was by your clock).
18. Now hold **◀ Turn** again: nothing moves, the log shows `ROT_JOG REFUSED BELOW_SAFE_HEIGHT`.
    Type `#PNUDGE-50` again: nothing moves, `CREEP REFUSED ROTARY_NOT_HOME` (the head may be turned
    below the safe-spin height). The banner offers **Home the head**.
19. Tap **Home the head** (or **Operate → Home**). The mast rises to the top first, then homes;
    the monitor shows `ALIGNMENT above the safe-spin height again …` as it passes 1100, then
    `HOME DONE`.
20. **Jog and release**, now that the head is home (so running on a moment is harmless): at the
    top, hold **▼ Down** for half a second and let go. It stops within a moment of letting go
    (`CREEP DONE LEASE_EXPIRED` or `CREEP ABORTED CANCEL`). With **Normal** power it moves at your
    seek floor (50 %); try one short hold. Then **▲ Up** back to the top.
21. **E-STOP during a move** (head still home). On the phone: hold **▼ Down** with one finger and
    tap **E-STOP** with another. On the laptop one mouse cannot hold a jog and click E-STOP at the
    same time, so start a move from **Operate** (Up, or Down) and click **E-STOP** while it runs.
    It stops and latches as always; an override, if one were on, ends with it. **Reset E-STOP**.
22. **Set home here**: with the head at home (after step 19, the home switch lamp lit), tap **Set
    home here…**, then **Tap again: the head is lined up**. The monitor shows `HOME SET by the
    operator …`, and Diagnostics shows the rotary home as `MANUAL` until the switch reads closed
    again at rest (then `SWITCH`). Only ever use this when you can see the head is lined up.
23. Put your height back: `#PSAFESPIN904` on the monitor (commissioning below measures it again).

## Part D — Commissioning, on your board as it is (Setup → Commissioning)

Your board is "unlocked on the bench" (`commissioned=1`). Running commissioning measures
everything again and marks it `commissioned=2`. Each step tells you what it will do before it
moves; nothing starts by itself. **Pause** stops the flow at any point and **Resume** continues
at the same step, even after a power cycle. An E-STOP pauses it too.

24. **Start**. Step **1 · Profile**: the 19:1 is already selected. Tick the five boxes, **Next step**.
25. Step **2 · Wiring check**. Nothing moves. With the mast down, the bottom lamp is lit. Press the
    top switch by hand: its lamp lights (and the board notes both switches closed while you hold
    it; that clears when you let go). "Runs backwards" must read **yes** for your lifter; leave it.
    Tick the box, **Next step**.
26. Step **3 · Lift test**: **Run the lift test**. The mast creeps up, continuously, at the profile's
    lowest power (45 %), and stops on the top switch. Result: `reached the top switch; the creep
    saw about 1180 ticks from the bottom switch (travel in use: 1150)`. **Next step**.
27. Step **4 · Rotary**: **Measure the rotary**. At the top the head turns three times, then homes.
    Result: `1185.x ticks/rev (spread 0.x %) … confident; home found`. **Next step**.
28. Step **5 · Calibration**: **Calibrate the lifter**. About six minutes, as on 2026-09-19; the bar
    and the text show which part it is on. Its first move is a creep **down** to the bottom
    switch (this proves the way down). Result: `travel 11xx ticks, lowest power up 2x % / down 1x %
    (stored)`. Your `#PTRAVEL1150` and `#PMINUP/#PMINDOWN 50` still win over it, as before.
    **Next step**.
29. Step **6 · Safe-spin height**. The one that needs your eyes. Read the red box on the page.
    - **a. Raise and turn the head**. The old height is cleared (`SAFESPIN=0 … it was 904`; only
      the top counts as clear from here until you store a new one), the mast goes to the top and
      the head turns to 90° there.
    - **b. Override on (60 s)**. Press it again whenever the countdown gets low.
    - **c.** Use **Lower 3 %** (35 ticks) down to about 80 % (the Height line reads about 920):
      you have already seen the head turn freely there. Below 920 use only **Lower 1 %** (12
      ticks; from the early-braking build of re-test 3 on, each step lands within a few ticks of
      its size and the log says `NUDGE came to rest … (asked -12)`; before, it ran on about 12
      ticks further), and
      after every step look at the lowest part of the head against the dome edge **and
      every part of the lifter frame it would pass over if it turned**, not only what is under it
      at 90°. Stop while it is still clearly clear, a finger's width at least. If you are not sure
      about the whole sweep, stop at 920: that height is proven.
    - **d. Use this height…**, then **Tap again: store it**. The board adds a margin of 3 % of
      travel (34 ticks) and says `SAFESPIN=… (saved; stored … : measured … + 34 margin)`.
    - **e. Raise and home the head**, then **End the override**.

    Compare the stored number with your old 904. If it came out lower and you have any doubt,
    set 904 back on Setup → Tuning; the measured number is only as good as the stop you chose.
    Tell me both numbers. **Next step**.
30. Step **7 · Acceptance**: **Run the acceptance**. Home, 0 %, 50 %, 100 %, home, then one gentle
    random action, all on the new numbers. Result: `home, 0 %, 50 %, 100 %, home and a random
    action all ran`. **Finish: mark commissioned**. The monitor shows `COMMISSIONED=2`, and the
    commissioning card shows seven green ticks.

If a step fails, its box turns red and says why in plain words, and the step can simply be run
again. Nothing moves on its own after a failure.

## Part E — Firmware upload from the page (Setup → Firmware)

31. With the periscope idle, **Setup → Firmware → Choose File**, pick
    `~/Desktop/uppity-build/UppityFirmware.ino.bin` (the file from step 2; on the phone use the
    laptop instead, or AirDrop the file to Files first), **Upload**. A banner says an update is
    running, the percentage climbs, then `Written. The board is restarting…`, and the page reloads
    itself on the restarted board. The monitor shows `OTA 100% done, restarting` and a fresh boot.
    Picking a wrong file (anything that is not an ESP32 app image) is refused before it is sent.
32. While the mast is moving, try the upload again: it is refused (`Refused while a motor is
    enabled or a job runs`).

## Part F — Optional: commissioning from a factory reset

This is the plan's full check of commissioning on a board that knows nothing. It costs you a
little set-up afterwards, so skip it if you would rather not.

What a factory reset clears: every v4 setting, including "runs backwards", travel, the rotary
numbers, your home WiFi network and the calibration. What survives: v3's stored values (motor
profile, minimum powers 50 %, safe-spin 904, rotary 1191 ticks per turn, all re-imported at the next
boot). After the reset the board is LOCKED and only on its own network, `R2Uppity-FB3F`, password
`Astromech`, at `http://192.168.4.1`.

33. Make sure you have the settings text from step 5.
34. **Setup → Firmware → Factory reset…** → **Erase everything and restart**.
35. Join the phone to `R2Uppity-FB3F` (password `Astromech`) and open `http://192.168.4.1`. The
    banner says LOCKED. **Setup → Commissioning → Start** and go through steps 1–7 as in Part D,
    with one difference: in step 2 press **Flip** so "Runs backwards" reads **yes** before step 3.
    (If you forget, step 3 stalls on the bottom stop for under a second and tells you to flip it;
    flip it and run step 3 again.)
36. Afterwards, put back the things commissioning does not measure. On **Setup → WiFi**: your home
    network name and password, then **Restart to apply**. On **Setup → Tuning**: light kit in
    random mode = Sparkle (your `#PRANDOMLIGHT=7`). Travel: commissioning leaves the calibrated
    figure in use (about 1140, between the switches); your old `#PTRAVEL1150` is in the backup if
    you prefer it.
    Anything else from the backup can go back through **Diagnostics → Configuration → Restore**,
    but do not paste the whole backup: its `#PSAFESPIN` and `#PROTARYTPR` lines would replace
    what you just measured.

## Send back

The log file (`~/Desktop/uppity-bench-phase4-log.txt`), and:
- step 4 (the migration line) and step 8 (did `Saved` appear);
- step 17: how many seconds between the override turning on and `OVERRIDE off`;
- steps 18 and 20: the refusals and how quickly the jog stopped on release;
- step 29: the stored safe-spin number, the height you stopped at, and your old 904;
- step 30: whether the acceptance passed; step 31: whether the page reloaded by itself;
- anything on the page that was hard to read or hard to hit, in either theme.

Still not run from earlier phases, if you get to them: two phones on the page at once, the
command UART (the Marcduino bus), the trigger inputs.

## Results 2026-10-03, first run (build df9b0716, Todd's periscope, from the laptop)

Flashed over USB (the board's house address had changed to 192.168.86.69). Worked: the settings
migration and every number intact; the page tour; settings saved with `Saved` from the board's own
reply; the two-tap guard on lowering the safe-spin height; both themes; the override lasting exactly
60 s (on at 776.858, `OVERRIDE off` at 836.858); a `#PNUDGE-50` under the override and a rotary jog
under the override at about 1100; both refused once the override ended
(`ROT_JOG REFUSED BELOW_SAFE_HEIGHT`, `CREEP REFUSED ROTARY_NOT_HOME`); **Home the head** raising
first and clearing the alignment; jogs stopping on release; **Set home here**; `#PSAFESPIN904` back.

**Failed, and it did damage: the head hit the lifter frame.** In step 13 the first Down stopped at
1100 as intended (`CREEP CLAMPED ROTARY_NOT_HOME` at 665.006), but the next presses were let through
(jobs 8 and 9 at 667–669 s, and again 14 and 15 at 722–723 s): the head, turned 90°, went down
past 1100 and into the frame. The cause was in the safety rules, not in the jog: a head at or below
the safe-spin height was treated as "inside the dome opening, therefore lined up", and once the
clamp had stopped the turned head exactly at the threshold, the next descent counted as starting
"inside". The same hole had been there since Phase 1 for two `:PP` presets in a row with the head
turned. The jog also had no stall exit, so it kept pushing while the button was held. Step 13 was
mine and said the board would not let the mast go lower; that was not true of the build.

E-STOP from the laptop: the button jumped half its width sideways when pressed (a general "pressed
button" style replaced the E-STOP's centring), which made clicks miss. Step 21 also asked for a held
jog and an E-STOP click at once, which one mouse cannot do.

Fixed in the next build (host tests 144; your sequence is now a test that fails on the old build and
passes on this one):
1. The board remembers that the head may be turned from the moment it is off home above the
   safe-spin height (or is turned below it under the override), and refuses every descent below
   that height until a home clears it. A turned head stopped at the threshold stays there; jog,
   nudge and `:PP` alike are refused with `ROTARY_NOT_HOME`, and the page shows "The head may be
   turned below the safe-spin height" with a **Home the head** button. `:PH` from that state rises,
   homes, then lowers. A head parked inside the dome at power-on can still go up a little and back
   down without homing, as before.
2. A held jog stops by itself if the mechanism stops moving: 700 ms on your 19:1 lifter, 800 ms on
   the rotary (`CREEP STALLED STALL` / `ROT_JOG STALLED STALL`).
3. Random mode with parameters (`:PM,…`) now homes before each random lift, as the other random
   modes already did; it had been relying on the same hole.
4. The E-STOP no longer moves when pressed, and also answers a plain mouse press.

## Before anything moves again

Look over what the hit may have done, as after 2026-09-19: the head on its mount and the coupler set
screw, the periscope tube, the frame where the head struck, and how much play the rotary now has.
Tell me what you find. If anything is loose or bent, fix that first; the firmware cannot make up for
a head that has shifted on its shaft.

## Re-test for the fix build (short; nothing here can reach the frame even if the fix failed)

1. Flash over USB, in the Terminal app, monitor closed, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity UppityFirmware
   ```
   ```
   arduino-cli upload --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
   ```
   Then the monitor as in step 3. The boot line shows a new build id; `#PSTATE` ends its second
   line with `alignment=ok`.
2. `#PSAFESPIN1100` on the monitor. **Operate → Up**, then **90°** (the head turns at the top).
3. **Rescue → Lifter jog → ▼ Down** (Slow): press and keep holding until it stops by itself at
   1100 (`CREEP CLAMPED ROTARY_NOT_HOME`), then let go. Do not press it again. (A click is too
   short: the jog ends when you let go, above 1100; then the first nudge in step 4 goes down to
   1100 and stops there, and only the second one is refused.)
4. On the monitor type `#PNUDGE-20`. Expected: nothing moves, `CREEP REFUSED ROTARY_NOT_HOME`,
   and the page's amber banner "The head may be turned below the safe-spin height …". (If the fix
   had not worked, this step could only move 20 ticks, to about 1080, which is clear.)
5. Type `:PP94` (94 % = 1081 ticks). Expected: nothing moves, `SEEK REFUSED ROTARY_NOT_HOME`.
   (Again at most 20 ticks if it were wrong.)
6. Tap **Home the head** on the banner. The mast rises to the top, the head homes, the banner goes
   and the monitor shows `ALIGNMENT above the safe-spin height again …` on the way up, then
   `HOME DONE`. Then `#PSAFESPIN904`.
7. E-STOP from the laptop: **Operate → Down** (head home, so the descent is fine), click **E-STOP**
   while it moves. The button stays where it is and reads `E-STOP LATCHED` on the first click.
   **Reset E-STOP**, **Operate → Up**, click E-STOP during the rise. Three times in all.

Send back the log.

## Results 2026-10-03, re-test (build 71955ce1)

The fix held. With the head turned 90° at the top and `#PSAFESPIN1100`:
- Step 3: the jog was a click rather than a hold, so it ended after 0.18 s (`CREEP DONE
  LEASE_EXPIRED`), at about 1115 ticks, before reaching 1100. My step 3 said "once" where it
  should have said "hold until it stops"; corrected above.
- Step 4, the "very, very slight" movement: the first `#PNUDGE-20` started above 1100, so it was
  allowed down to 1100. The log shows it crossing 1100 (`ALIGNMENT suspect` at 150.088) and
  braking on that same millisecond; `CREEP CLAMPED ROTARY_NOT_HOME` follows 60 ms later, the
  creep's fixed brake period. The mast came to rest at 1088: the creep starts braking on the
  first tick at or below 1100, and at 15 % the mast's momentum and weight carry it another 12
  ticks while the brake bites. The second and third `#PNUDGE-20` and `:PP94` were refused with
  nothing moving. That is the policy fix working on your hardware.
- Step 6: **Home the head** rose from 1088 to the top, homed and parked; `#PSAFESPIN904` back.
  The monitor said `ALIGNMENT ok again (the head is home)` while the mast was still rising past
  1100, before the home. The words were wrong (the flag clears when the mast is back above the
  safe-spin height); fixed below.
- Step 7: the E-STOP latched on the first click every time from the laptop and the button stayed
  put. **But during both descents the mast did not stop:** `LIMIT BOTTOM made at 13` came 121 ms
  after the latch in the first and 505 ms after it in the second, from about 600 ticks. During the
  rise it stopped.

Why the E-STOP let the mast run on. Pressing it brakes the motors at once (both driver inputs
low, the TB9051FTG's "short brake"), but within a few milliseconds the board also switched the
drivers off through the I2C expander, and a switched-off driver leaves the motor disconnected:
it coasts. The brake only works while the drivers are on. Your lifter holds its position at rest
with the drivers off, but once it is moving down the mast's weight keeps the screw turning, so a
falling mast free-wheels to the bottom. An ordinary stop never showed this, because after every
move the drivers stay on, braking, for 2 s (`DRIVERS disable` two seconds after each move in the
log); only the E-STOP switched them off straight away. The simulator modelled your screw as
self-locking, so no host test could see it.

Fixed in this build (host tests 151; 1,174,727 bytes, 59 %):
1. After an E-STOP the drivers stay on, braking, until both motors have been still for 0.1 s,
   then switch off; 2 s at most, the same as after any ordinary stop. A new log line says how
   it went: `ESTOP braked to a stop: lifter -NN ticks, rotary +0 since the latch; drivers off
   after NNN ms`. If the drivers were already off when it was pressed (the mast at rest more than
   2 s after a move), nothing changes and there is no new line.
2. The simulator now has your lifter's behaviour: it holds at rest, and falls on when the
   drivers are off while it moves. The bench sequence is a host test, and it fails on the
   previous build (the mast coasts on) and passes on this one.
3. The alignment line now says `ALIGNMENT above the safe-spin height again at NNNN: the head may
   turn here, and must home before it goes lower` when the mast rises past the safe-spin height,
   and keeps `ALIGNMENT ok again (the head is home)` for a home that clears it.

Not changed yet, on purpose: a creep (jog or nudge) that stops at the safe-spin height for a
turned head comes to rest a little below it, 12 ticks at the Slow power. With your 904 that
means about 892. That is within the margin commissioning adds (34 ticks on your lifter) and well
above where the head has hit the frame (about 650). A faster jog would go further. The fix is to
start braking early by the braking distance at the creep's speed, the way the seeks already use
your calibration. Re-test 2 measures that distance at full speed; I will set the early start from
your numbers rather than guess it.

## Re-test 2 for the E-STOP build (the head stays home: nothing here can reach the frame)

Every move below is an ordinary up or down with the head home. If the fix did not work, the worst
that happens is what happened today: the mast runs on to the bottom switch. Nothing here turns
the head.

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/uppity-spinner-phase-4-1e5680
   ```
   Success looks like `Fast-forward` and a list of files.
2. Flash over USB, monitor closed, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity UppityFirmware
   ```
   ```
   arduino-cli upload --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
   ```
   The compile ends with `Sketch uses 1174727 bytes (59%)` (a few bytes either way is fine); the
   upload with `Hard resetting via RTS pin...`.
3. The monitor, in its own Terminal window (a new log file, so today's stays as it is):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-retest2-log.txt
   ```
   The boot line shows a new build id (not `71955ce1`), and the `BOOT` line reads
   `lifter=KNOWN rotary=SWITCH_LIVE` (the mast on a switch, the head home). If it says anything
   else, tap **Operate → Home** first and wait for it to finish.
4. **Operate → Up**, wait for 100 %.
5. **Operate → Down**, and click **E-STOP** while the mast is still going down (earlier is better;
   the whole descent takes under a second). Expected: it stops almost at once, and the monitor
   shows `ESTOP LATCHED (source 3)` and, a fraction of a second later, the new line `ESTOP braked
   to a stop: lifter -NN ticks, rotary +0 since the latch; drivers off after NNN ms`, with no
   `LIMIT BOTTOM made` after it (unless it was already at the bottom). If it runs on to the bottom
   again, stop here and send me the log.
6. **Reset E-STOP**. **Operate → Up**, and click **E-STOP** while it rises. Expected: it stops,
   and the line reads `lifter +NN ticks`.
7. **Reset E-STOP**, then steps 4 and 5 twice more, clicking E-STOP at different points of the
   descent.
8. **Reset E-STOP** and **Operate → Home**.

Send back the log (`~/Desktop/uppity-bench-phase4-retest2-log.txt`). The numbers in the `ESTOP
braked to a stop` lines are your lifter's real
braking distances at full speed; they are what I will use to make the safe-spin stop brake early.
After that, Parts D–F are as written above. Your board already runs unlocked, so there is no hurry
for Part D; if you run it, stop at 920 ticks in step 29 unless you can see that the whole sweep of
the head clears lower.

## Results 2026-10-04, re-test 2 (build db08a868)

The E-STOP fix works on the droid. Four presses during full-speed descents (about 1.5 ticks/ms)
and two during rises (about 1.2 ticks/ms), the head home throughout:

| Press | Direction | Lifter after the latch | Drivers off after | Came to rest at |
|---|---|---|---|---|
| 65.226 | down | −54 ticks | 227 ms | 206 |
| 71.134 | down | −59 ticks | 278 ms | 378 |
| 93.915 | up | +32 ticks | 186 ms | 602 |
| 99.500 | down | −53 ticks | 220 ms | 256 |
| 113.804 | up | +31 ticks | 155 ms | 587 |
| 119.216 | down | −60 ticks | 282 ms | 435 |

No `LIMIT BOTTOM made` after any of them; before this build both descents ran on to the bottom
switch. Braking distance is about the speed times 40 ms going down and 27 ms going up, the same
proportion as the 15 % creep that stopped 12 ticks past 1100 on 2026-10-03. (The board booted twice
when the monitor opened: the USB adapter resets it as the port opens, and the first boot's lines
were already in its buffer. The WiFi took two retries to join; the backoff did its job.)

Changed in the next build (host tests 155; 1,175,979 bytes, 59 %):
1. **Jogs and nudges start braking early**, by the distance the mast will run on: their speed over
   the last 16 ms times a braking time taken from your own calibration's stopping distances (the
   seeks already brake early from the same table). A nudge now lands within a few ticks of its
   size, and the log says where: `NUDGE came to rest -20 ticks from where it started (asked -20)`.
   With no calibration nothing changes.
2. **A jog or nudge stopping a turned head at the safe-spin height comes to rest at it or a few
   ticks above it**, instead of about 12 below: the early start has a quarter extra there, so any
   error lands on the safe side. The log says `CLAMP came to rest at 1103, +3 from the safe-spin
   height 1100`.
3. **Stopped there, it stays there.** Every further descent is refused (`ROTARY_NOT_HOME`, the page
   offers **Home the head**) until the head homes, the mast rises at least 20 ticks, or the
   safe-spin height is changed. Without this, a second press starting from rest just above the
   height could not brake early enough and would end a tick or two below it.
4. `#PCAL` ends with a new line, `CALBRAKE down=NNms up=NNms`: the braking times jogs and nudges use.

## Results 2026-10-04, Part D (build db08a868, the dome off)

Commissioning ran through all seven steps and marked the board `COMMISSIONED=2`. What the log shows:

- **Step 2 (wiring):** both switches read correctly, and holding the top switch with the mast down
  showed the expected both-made fault until you let go. While nothing was driven the count also
  moved about 70 ticks and the bottom switch opened and closed once (412–417 s); I assume that was
  the mast being handled. If it was not, tell me.
- **Step 4 (rotary): wrong value stored, now fixed.** It stored 1183.0 ticks/rev for a rotary that
  measures 1185.5. The head started parked on the home switch, and the test counted that resting
  spot as a switch edge, so the first "turn" ran from the park spot to the next edge: 1179, then
  1185 and 1185, average 1183.0. The September test started off the switch, which is why it never
  showed. The next build ignores the switch it starts on. The effect of 1183.0 is small (0.2 %,
  under a degree per turn, and every home corrects it), and re-test 3 measures it again.
- **Step 5 (calibration): travel measured between the wrong points, now fixed.** It stored travel
  1192, measured from where the mast rests at the bottom to where it rests at the top. The count is
  referenced where the switches close (0 at the bottom switch, the travel at the top switch), and
  between those points your lifter measures about 1138: it runs on 25–30 ticks past each switch
  while braking. With the calibrated figure in use, the count would jump by about 54 at every
  switch. Your `#PTRAVEL1150` overrides it, so nothing changed for you; don't send `#PTRAVEL0`
  until you have calibrated again on the next build, which measures between the switch closings
  and ends with 0 at the bottom switch rather than at the resting spot under it. The lowest powers
  (25 % up, 15 % down) came out as before.
- **Step 6 (safe-spin):** measured with the dome off, so the stored 897 (863 plus the 34-tick
  margin) is a placeholder until the dome-on run. The eight **Lower 3 %** presses moved about 50
  ticks each, not 35, and the five **Raise 1 %** about 23 each, not 12: the run-on the early-braking
  build removes. Working back from where you stopped (863), the turned head got down to roughly 750
  before you raised it, about 100 ticks above where it touched the frame on 2026-10-03. Nothing in
  the log suggests it touched anything (no stall, no fault).
- **Step 7 (acceptance)** and everything after it (presets, the rotary presets, a spin stopped with
  **Soft stop**, home, lower, the theme switch) ran normally. A `COMMISSION=… passed` line can
  appear just before the last lines of the move it reports on; the board prints its own messages
  as they happen and the motion lines a moment later, so that order is cosmetic.

Changed in the next build (host tests 157; 1,176,139 bytes, 59 %), on top of the early braking:
1. The rotary test ignores the switch it starts on and counts only edges it drives into. A test
   starting parked on the switch now measures within 1 tick of the true value; on the previous
   build it came out 2 low, as on your board.
2. The calibration measures travel between the switch closings and ends with 0 at the bottom
   switch. On a simulated lifter that runs on past its switches the way yours does, the next pass
   over each switch now lands within 2 ticks of where the count expects it. Before, it was off by
   the run-on.

## Re-test 3: early braking, installed from the page (nothing here can reach the frame)

This also does Part E step 31: the new build goes in through the page's own firmware upload. The
head is home for steps 5–6. In step 7 a turned head is lowered only by one bounded nudge from the
top; if the new code failed completely it would stop at about 1040, well above the 920 where you
have watched the head turn freely.

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/uppity-spinner-phase-4-1e5680
   ```
   Success looks like `Fast-forward` and a list of files.
2. Build the file the page will send, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1176139 bytes (59%)` (a few bytes either way is fine).
3. The monitor, in its own Terminal window (a new log file):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-retest3-log.txt
   ```
4. On the laptop's browser, open the page (`http://192.168.86.69`, or the address in the monitor's
   `WIFI joined` line). With the periscope idle: **Setup → Firmware → Choose File**, pick
   `UppityFirmware.ino.bin` in the `uppity-build` folder on your Desktop, **Upload**. A banner says
   an update is running, the percentage climbs, then `Written. The board is restarting…`, and the
   page reloads itself. The monitor shows `OTA 100% done, restarting` and a fresh boot with a new
   build id (not `db08a868`). If the page upload fails, note what it said and flash over USB
   instead (monitor closed, same folder as step 2):
   ```
   arduino-cli upload --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
   ```
5. Type `#PSAFESPIN904`. Answer: `#PSAFESPIN=904 (saved)`. That is your long-standing value; the
   897 from Part D was measured with the dome off, so it stands in only until the dome-on run.
6. Type `#PCAL` on the monitor. Expected: `CAL=in use … travel=1192 …` (today's calibration; the
   1192 is the rest-to-rest figure, unused while `#PTRAVEL1150` is set), the `CALUP`/`CALDOWN`
   table, and the new `CALBRAKE down=NNms up=NNms (jogs and nudges start braking this far ahead)`.
   From the E-STOP numbers I expect about 40 down and 27 up; tell me what it says.
7. Type `#PROTARYTEST`. The mast goes to the top if it is not there, and the head turns about four
   times and stops just past home (the board knows where home is). Expected: `ROTARY stored:
   1185.x ticks/rev confident=1 …` (Part D stored 1183.0). If it says anything else, send it to me
   before going on.
8. **Operate → Up**, then on the monitor, one at a time, waiting for each to finish:
   `#PNUDGE-20`, `#PNUDGE-50`, `#PNUDGE+20`, `#PNUDGE-12`. After each, about a second later:
   `NUDGE came to rest -NN ticks from where it started (asked -20)` and so on. Expected: each within
   about 4 ticks of what was asked (before this build: about 12 more).
9. `#PSAFESPIN1100`, **Operate → Up**, then **90°** (the head turns at the top). Then `#PNUDGE-100`.
   Expected: `CREEP CLAMPED ROTARY_NOT_HOME`, then `CLAMP came to rest at 11NN, +N from the
   safe-spin height 1100`, with N zero or a small positive number (I expect under 10; before this
   build it was −12), and the page's banner "The head may be
   turned at or below the safe-spin height". Then `#PNUDGE-20`: nothing moves, `CREEP REFUSED
   ROTARY_NOT_HOME`.
10. Tap **Home the head**: the mast rises, the head homes, the banner goes. Then `#PSAFESPIN904`.

Send back the log (`~/Desktop/uppity-bench-phase4-retest3-log.txt`). Part E step 32 (an upload
refused while a motor runs) and Part F (factory reset) are optional.

## Results 2026-10-04, re-test 3 (build b14aa631)

Everything did what it should:
- **Firmware upload from the page (Part E step 31):** installed, and the board restarted on the new
  build. During the upload the log printed a `TICK` line every second showing the control loop
  pausing up to 10 ms at a time: writing flash briefly stops the processor reading its own program.
  The motors are held for the whole upload (`OTA 0% (motors held)`), so it does no harm.
- `#PCAL`: `CALBRAKE down=32ms up=20ms`. The table shows the braking time growing with speed (going
  down 27 ms at 15 % to 38 ms at 85 %, going up 5 to 28 ms), which matches the E-STOP's 37–40 ms
  down and 27 up at full speed. 32 and 20 are the middle of the table.
- `#PROTARYTEST`: `ROTARY stored: 1185.7`, from four clean turns (1187, 1184, 1186 between edges).
  The Part D value of 1183.0 is replaced.
- Nudges: −21 for −20, −51 for −50, −13 for −12, +16 for +20 (before this build each ran on about
  12 more). Going up it stops a few ticks short: at low speed the mast stops quicker going up than
  the middle of the table says. A raise step slightly smaller than asked is harmless.
- The clamp: the turned head came to rest at 1104, 4 above 1100. The next nudge was refused,
  **Home the head** cleared it, and 904 went back.
- `ALIGNMENT suspect` straight after `#PSAFESPIN1100` was correct, though unexpected. The rotary
  test had stopped the head just past home, about 11° off (the home window is 10°), and the mast
  at 1087 was now below the new height, so the board treated the head as possibly turned.

Changed in the next build (host tests 158; 1,176,195 bytes, 59 %):
1. `#PROTARYTEST`, and the Diagnostics page's rotary test that sends it, homes the head when it is
   done, as commissioning already did.
2. The two rotary lines round the same way (they said 1185.7 and 1185.6 for the same figure).

## Later, with the dome on: commissioning again for the real safe-spin height

This installs the newest build, then measures everything again: the rotary, the calibration (the
first on the droid with travel counted between the switch closings) and, with the dome on, the
safe-spin height. The board stays unlocked throughout.

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/uppity-spinner-phase-4-1e5680
   ```
2. Build it, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1176195 bytes (59%)` (a few bytes either way is fine).
3. The monitor, in its own Terminal window:
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-domeon-log.txt
   ```
4. **Setup → Firmware → Choose File**, `UppityFirmware.ino.bin` in the `uppity-build` folder on
   your Desktop, **Upload**. The board restarts with a new build id (not `b14aa631`).
5. **Setup → Commissioning → Start over**, tap again to confirm, then **Start**. Steps 1–7 as in
   Part D (steps 24–30 above).
6. Step 4 should store about 1185.5 ticks/rev. Step 5 should report travel about 1140 (between the
   switches) instead of 1192; tell me the number. Once we have seen it, `#PTRAVEL0` can replace
   your `#PTRAVEL1150`, so the height reads the same whichever switch it last passed.
7. Step 6 with the dome on: **Lower 3 %** and **Lower 1 %** now move what they say (the log shows
   `NUDGE came to rest … (asked -35)`). You saw the head clear at 920 on 2026-09-20; if the dome
   was on that day, **Lower 3 %** is fine down to 920. If it was off, or you are not sure, use
   **Lower 1 %** all the way from the top. After every step, watch the head against the dome
   opening and every part of the frame it would pass over if it turned. Stop while it is still
   clearly clear.
8. Send me the log and the stored number.

## Results 2026-10-04, dome-on commissioning (build 77cbfb02)

Installed from the page again, then all seven steps with the dome on: `COMMISSIONED=2`.
- **Step 4 (rotary):** 1185.0 ticks/rev, spread 0.3 %; edges 1184, 1184 and 1187 apart, and the
  switch it started on was ignored. A home from three turns away later found the switch within 4
  ticks of where three turns of 1185 put it.
- **Step 5 (calibration): the travel fix works.** Travel 1137, spread 0.1 %, measured between the
  switch closings (1192 before). After it the count is 0 at the bottom switch's closing point (the
  mast rested at −33), and the top switch then closed at 1135: the two switches agree within 2
  ticks. Lowest powers 25 % up and 15 % down, as before.
- **Step 6, the real safe-spin height: 878** (you stopped at 844, plus the 34-tick margin). Each
  **Lower 3 %** came to rest 30–34 ticks down and each **Raise 1 %** 9 up, so the early start now
  errs a few ticks short both ways. That is the safe side, and the log shows the real distance.
  The turned head went down to about 800 before you raised it to 844.
- **Step 7** and everything after it (home, lower, up, the 90/180/270 presets, a spin stopped with
  **Soft stop**, home, lower) ran normally.

Two things this run showed:
1. **Power-up on the bottom switch counted from the wrong point.** The count started at 0 where the
   mast rests, about 33 ticks below the switch's closing point, so the height read about 33 high
   until the mast passed a switch. The lift test right after power-up shows it: the top switch
   closed at 1172 instead of about 1137. In that window the head could have turned about 33 ticks
   lower than 878 means, roughly 845 on the true count. That is still above the 829 where you saw
   it clear, but with about 16 ticks of margin instead of 34. The next build fixes it: the
   calibration records the deepest the mast settles below the closing point, and power-up on that
   switch starts the count there. After an ordinary Down the mast rests a little higher (about 22
   below), so after power-up the count may read up to about 11 low, never high. Low is the safe
   direction. The calibration you have predates this, so it needs one
   recalibration to measure it (re-test 4).
2. **`#PTRAVEL0` is now right for you.** The calibrated 1137 agrees with both switches. With 1150
   the count jumps about 15 at every pass of the top switch. One consequence: 878 was measured
   while the top switch had just set the count to 1150, so it read about 15 high. With 1137 in use,
   878 sits about 15 above what commissioning intended. That is the safe side, so leave it.

Changed in the next build (host tests 161; 1,176,647 bytes, 59 %): the calibration measures how far
below the bottom switch's closing point the mast settles (stored with the calibration; `#PCAL` shows
it as `CALREST`, the boot line as `rest=`), and power-up on the bottom switch starts the count at
minus that. A calibration stored before this keeps the old behaviour until the next one.

## Re-test 4: the power-up height (the head stays home: ordinary moves only)

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/uppity-spinner-phase-4-1e5680
   ```
2. Build it, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1176647 bytes (59%)` (a few bytes either way is fine).
3. The monitor, in its own Terminal window:
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase4-retest4-log.txt
   ```
4. **Setup → Firmware → Choose File**, `UppityFirmware.ino.bin` in the `uppity-build` folder on
   your Desktop, **Upload**. The boot line now says `CALIBRATION=loaded travel=1137 … rest=0`
   (not measured yet).
5. Type `#PTRAVEL0`. Answer: `#PTRAVEL=0 (saved)`; the calibrated 1137 is in use from now on.
6. Type `#PSC`: the lifter calibration, about six minutes, as in commissioning step 5 (the head
   homes first if it needs to; E-STOP stops it). It ends with `CAL stored: travel=113x … rest=NN`.
   I expect `rest` about 33; tell me the number.
7. **Operate → Down** and let it reach the bottom. Then type `#PRESTART`: the board restarts and
   nothing moves. The boot line says `rest=NN`.
8. **Operate → Up**. Expected: `LIMIT TOP made at` somewhere from about 1120 to 1140 (this morning
   it said 1172, 35 high). Up to about 11 below 1137 is expected: the board now starts from the
   deepest resting spot, so it errs low, which is the safe direction.

Send back the log (`~/Desktop/uppity-bench-phase4-retest4-log.txt`).
