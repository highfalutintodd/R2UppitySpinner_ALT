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
     `ALIGNMENT suspect: …` when that starts and `ALIGNMENT ok again` when a home clears it.
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
    the monitor shows `ALIGNMENT ok again`.
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
      ticks), and after every step look at the lowest part of the head against the dome edge **and
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
    figure in use (about 1181); your old `#PTRAVEL1150` is in the backup if you prefer it.
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
3. **Rescue → Lifter jog → ▼ Down** (Slow), once: it stops at 1100 (`CREEP CLAMPED
   ROTARY_NOT_HOME`). Let go. Do not press it again.
4. On the monitor type `#PNUDGE-20`. Expected: nothing moves, `CREEP REFUSED ROTARY_NOT_HOME`,
   and the page's amber banner "The head may be turned below the safe-spin height …". (If the fix
   had not worked, this step could only move 20 ticks, to about 1080, which is clear.)
5. Type `:PP94` (94 % = 1081 ticks). Expected: nothing moves, `SEEK REFUSED ROTARY_NOT_HOME`.
   (Again at most 20 ticks if it were wrong.)
6. Tap **Home the head** on the banner. The mast rises to the top, the head homes, the banner goes
   and the monitor shows `ALIGNMENT ok again`. Then `#PSAFESPIN904`.
7. E-STOP from the laptop: **Operate → Down** (head home, so the descent is fine), click **E-STOP**
   while it moves. The button stays where it is and reads `E-STOP LATCHED` on the first click.
   **Reset E-STOP**, **Operate → Up**, click E-STOP during the rise. Three times in all.

Send back the log. After that, Parts D–F are as written above. Your board already runs unlocked,
so there is no hurry for Part D; if you run it, stop at 920 ticks in step 29 unless you can see that
the whole sweep of the head clears lower.
