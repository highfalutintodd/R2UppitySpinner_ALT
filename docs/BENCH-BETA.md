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
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-beta-log.txt
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
   ;W3;S4#PSTATE
   ```
   (WCB3 writes `#PSTATE` out of its serial port 4, into the periscope's command port.) *Success:*
   the periscope's `STATE=…` lines in the monitor. This is the first time v4's command port has
   been driven by another board. If nothing arrives, stop here and tell me.
9. **The same move over the wire and the mesh at once (step 19 of Phase 5).** On WCB1's console,
   one line:
   ```
   ;W3;S4:PP100^;W5:PP100
   ```
   *Success:* the mast rises to the top **once** (one `SEEK START`). If the wire's copy came first,
   the monitor's `MESH :PP100 from 1` line ends `(a duplicate of the same line on the serial bus:
   ran once)`; if the mesh's came first, the wire's copy is dropped quietly. Then, in the monitor:
   ```
   #PSTATUS
   ```
   and its `SERIAL=` line shows `duplicates=1` in that case. Up is always allowed, so even two runs
   would be harmless.
10. **Down over the wire.** On WCB1's console:
    ```
    ;W3;S4:PH
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

## Part 3 — your own WiFi password (needed before the droid goes out anyway)

14. On the iPhone page: **Setup → WiFi**, the board's own network password: type a new one (8 to
    32 characters; write it down somewhere safe), **Save**, then **Restart to apply** (tap twice).
    Rejoin the iPhone to `R2Uppity-1390` with the **new** password and open `http://192.168.4.1`.
    *Success:* no warning banner, and in the monitor the boot shows `password <yours>` and no
    `WIFI WARNING` line. **Never type the new password into the monitor:** it would land in the
    log file. If you cannot get back in, type this into the monitor to put the published default
    back (it is no secret), then try again:
    ```
    #PAPPASS=Astromech
    ```
    ```
    #PRESTART
    ```

## Part 4 — a long run

15. **Random medium for 30 minutes.** On the page: **Operate → Random medium**. Let it run; watch
    now and then. Then **Stop**, and in the monitor:
    ```
    #PSTATUS
    ```
    *Success:* no `FAULT` lines in the log, every turn at the top or above 878, every descent below
    878 after a home, and in `#PSTATUS` the `MESH=` line still `wcbs=3/3`, `txFail=0`.

## Part 5 — a fresh install from the full image (what every tester does first)

Two ways; tell me which you can do.

- **A. On a spare ESP32 board**, if you have one (any ESP32-WROOM-32 dev board will do for this
  part): flash `uppity-spinner-alt-4.0.0-beta.1-full.bin` at `0x0` from
  [espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/) in Chrome, then join its
  `R2Uppity-XXXX` network and open the page. *Success:* the board boots `LOCKED`, the page shows
  the commissioning card and the default-password banner. Nothing can move on a bare board.
- **B. On the droid**, which erases everything stored on it (your settings, the calibration, the
  mesh settings and WordNet's details) and then runs commissioning from step 1 with the dome on,
  as on 2026-10-04. About 45 minutes. I would write that procedure separately, starting with saving
  your `#PCONFIG` so nothing is lost. Only if A is not possible.

## Send back

The log `uppity-bench-beta-log.txt` from your Desktop, WCB1's console for steps 9, 11 and 13, and
which way Part 5 can go.
