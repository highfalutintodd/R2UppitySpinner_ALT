# Bench procedure — Phase 3 (WiFi, web page, OTA)

For Todd. Phase 3 puts the periscope back on the phone: an access point that is always up, an
optional join to your home network, one web page (Operate + a console) with an E-STOP that
lands the moment it is pressed, and firmware updates over WiFi. Nothing in the motion core or
the command layer changed; everything from Phases 1 and 2 behaves as before.

## Flash (USB this once; after this, OTA works)

Same three lines as before, one at a time in the Terminal app, monitor closed:
```
cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
arduino-cli compile --profile uppity UppityFirmware
arduino-cli upload  --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
```
Success looks like `Sketch uses 1135007 bytes (57%)` (the WiFi stack is most of it) then
`Hard resetting via RTS pin`. Then the monitor:
```
arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase3-log.txt
```

## What is different, in one breath

The board raises its own WiFi network `R2Uppity-XXXX` (XXXX from its address), password
`Astromech`, address `192.168.4.1`, also `http://uppity.local`. Joining it on a phone pops the
page up by itself (captive portal). If you store your home network (`#PSSID=` / `#PPASS=`), the
board also joins that in the background, without ever dropping its own network, and the page is
then at `http://uppity.local` from any device in the house. Every line the firmware prints
also goes into a log the page shows, so the laptop is no longer needed to see what it says.
Updates: `arduino-cli upload` over the network, or the page's update endpoint. Both are refused
while a motor is enabled.

## Boot checks

1. Power on. New lines after `CONTROL_TASK=...`:
   ```
   SETTINGS=migrated from schema 2 (80 bytes) to 3     (once; the WiFi fields were added; a PERSIST=saved settings line follows)
   WIFI AP "R2Uppity-XXXX" password "Astromech" (the default; #PAPPASS=<yours> to change) -> http://192.168.4.1  (http://uppity.local)
   WIFI no home network stored (#PSSID=<name> #PPASS=<password> #PRESTART to join one)
   READY (type #PHELP)
   ```
   The `CONFIG ...` line must still show your numbers (inverted 1, travel 1150, safe-spin 904,
   1185.5 ticks/rev, commissioned 1). `#PSTATUS` ends with a `WIFI=ap "R2Uppity-XXXX"
   192.168.4.1 clients=0 ...` line.
2. `#PSTATE` while idle: `tick=[max Nus late 0 ...]`. The radio runs on the other core; the
   control tick should look the same as before (about 45 µs).

## The page (phone on the periscope's network)

3. On the phone, join WiFi `R2Uppity-XXXX` with password `Astromech`. iPhones show the page by
   themselves ("Log In" sheet); if not, open Safari at `http://192.168.4.1`. The log on the
   laptop shows nothing special for a join; `#PSTATUS` shows `clients=1`, and the status LED
   turns **blue** (solid blue = home, blue/red = head turned) while a phone is connected.
4. The header shows the state pill (`IDLE`), height `0%`, rotation `0°`, and the red E-STOP
   button sits at the bottom of the screen on every scroll position. The Console card at the
   bottom shows the same lines as the serial monitor, live.
5. **Lifter** card: tap **Up**. The monitor and the page's console both show `QUEUED=SEEK`, the
   pill goes `MOVING` then `IDLE`, height climbs to `100%`. Tap **50%**, then **Down** (`:PH`).
6. **E-STOP during a move**: tap **Up**, and while it is moving tap the red **E-STOP**. Expect
   `ESTOP LATCHED (source 3)` on the monitor (3 = web), the motor stopping at once, the pill
   `STOPPED`, and a red banner "E-STOP is latched" with a **Reset E-STOP** button. Tap it:
   `ESTOP released`, `IDLE`. This is the check that matters most in this phase.
7. **Rotary** card: **Home**, **90°**, **270°**, then **Spin ▶** and **Soft stop** (the spin
   stops, the head stays where it is; `STOP=web` on the monitor). Change the speed segment to
   Gentle and tap 180°: the command sent is `:PA180,40`.
8. **Show** card: **Random gentle**, watch a couple of actions (the pill "RANDOM ON" shows in
   the card), then **Stop show**. The light pills: tap **Search** (`LIGHT=5`), **Off**.
9. **Console** card: type `#PSTATE` and Send; the answer appears in the log within a second.
   Type `:PP25` and Send. Type `xyz` and Send: the page refuses it ("a command starts with :P
   or #P") without bothering the board.
10. Walk out of range (or turn WiFi off on the phone): the pill goes dashed amber `NO LINK`
    within two seconds; nothing on the board changes. Come back: it recovers by itself.
11. Theme: the `◐` button switches to the dark theme and back; the phone remembers it.
12. **Two phones**: join with a second device. Both pages update; the `clients=` count is 2.
    A command from either shows on both consoles.

## Home network (optional, needs your WiFi name and password)

13. On the monitor: `#PSSID=<your network name>` then `#PPASS=<its password>` (each answers
    `(saved; takes effect after #PRESTART)`; the password is never printed), then `#PRESTART`.
    After the boot: `WIFI home network "..." stored; joining in the background`, and within
    about ten seconds `WIFI joined "...": http://uppity.local (192.168.x.y, -NN dBm); the AP
    stays up too`. From the laptop on the house WiFi, open `http://uppity.local`. If the join
    fails you get `WIFI not on "..." (reason N); retry in 15s, the AP is still up` and the
    retries back off to five minutes; the periscope's own network is unaffected. A retry is
    skipped while a phone is on the AP (`WIFI home-network retry paused ...`).
    `#PWIFIRESET` puts everything back to the access point only.

## OTA (firmware over WiFi)

14. From the laptop on the same network as the board (the house WiFi once the board has joined
    it, or the periscope's own network), with the periscope idle. First build the image into a
    folder on the Desktop (this also refreshes the build after a merge):
    ```
    cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
    ```
    ```
    arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
    ```
    Then send it over WiFi. The address is the one `#PSTATUS` prints after `home=`; on the
    house network it was `192.168.86.90` on 2026-09-27 (on the periscope's own network it is
    always `192.168.4.1`). The password is the access point password (`Astromech` unless you
    changed it with `#PAPPASS`), not your home WiFi password:
    ```
    python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.4/tools/espota.py -r -i 192.168.86.90 -p 3232 -a Astromech -f ~/Desktop/uppity-build/UppityFirmware.ino.bin
    ```
    Success looks like `Uploading: [====...] 100%` then `Result: OK`; the monitor shows
    `OTA 0% (motors held)`, `OTA 10%` ... `OTA 100% done, restarting`, and the board reboots
    into the new build. If a motor is enabled or a job is running the board does not answer
    the upload at all and the tool reports `No response from the ESP`: wait until it is idle
    and try again. (`arduino-cli upload` can do the same only with `--protocol network`; the
    command in the first version of this doc lacked it and tried the USB uploader instead.)
15. While a move is running, a `POST /update` from the page's API answers
    `update refused: a motor is enabled or a job is running`. (No button for it yet; Phase 4's
    Setup page gets one.)

## Send back

The log file, and: the boot `WIFI` lines, whether the phone joined and the page popped up by
itself, step 6 (E-STOP from the page while moving) and step 14 (OTA). If the page ever shows
`NO LINK` while the phone is still on the network, the `#PSTATUS` line at that moment.

## Results 2026-09-27 (Todd's periscope)

Worked: the access point (`R2Uppity-FB3F`), the phone joined, every Operate button drove the
periscope, soft stop cancelled a spin and a rotation, random mode from the page with the light
kit following, the safety clamp from the web (`SEEK CLAMPED 287 -> 904 reason=ROTARY_NOT_HOME`),
theme and settings saved, schema-3 migration with everything intact, home network stored and
joined in about a second with the access point still up, E-STOP and reset from the page while
idle. Not run: two phones, the trigger inputs.

Failed: **E-STOP from the page never landed while the lifter was moving** (it did while idle,
and soft stops landed mid-rotation). Every request sent during a lifter move arrived the moment
the move ended. A ping from the laptop on the house network lost 13 packets in a row during a
`:PP100` raise and spiked at driver enable/disable moments, with 3 % loss overall at -70 dBm;
the rotary spin passed clean. Two causes stacked: the radio loses packets around lifter
activity (pulsed drive current on the supply), and the single-threaded web server held a
connection whose request never arrived for up to five seconds, blocking everything behind it.
Also: the page never showed `NO LINK` (no request timeout; an iPhone leaves a request pending
for a minute), and the OTA command in the first version of this doc used the wrong uploader.

Fixed in the next build: a dedicated E-STOP listener on port 81 in its own task, the page
firing at both ports on fresh connections every 300 ms until the status confirms the latch
(button reads `SENDING E-STOP…`, then `E-STOP LATCHED`, or `NO ANSWER: USE THE POWER SWITCH`
after eight seconds); the web server drops a connection that has sent nothing for 400 ms;
two-second timeouts on the polls and no stacked polls; WiFi now starts before the control task
(the 73 late ticks at boot were WiFi bring-up); a changed `#PSSID` waits for the restart as the
doc says; the OTA step above corrected.

## Re-test for the fix build (short)

1. Boot: `#PSTATE` shows `late 0` (WiFi no longer counts against the control tick).
2. `#PSTATUS`: the `WIFI=` line ends with `estopPort=81 hits=0 stalledDropped=0`.
3. On the phone, tap **Up**, and while the mast is moving tap **E-STOP**. The button must go
   `SENDING E-STOP…` at once and then `E-STOP LATCHED` within a second or two, with
   `ESTOP LATCHED (source 3)` on the monitor **while the mast is still moving** and the motor
   stopping. Do this three or four times (Up, Down, a 50 % seek). `#PSTATUS` afterwards:
   `hits=` counts the port-81 latches, `stalledDropped=` the stuck connections the server
   cleared. Both numbers are useful to me.
4. Turn WiFi off on the phone (or walk away): the pill goes to `NO LINK` within about six
   seconds. Back on: it recovers.
5. Send commands from the page during lifter moves (tap 25 % while rising): they should now
   land during the move, not after it, most of the time. If some still wait for the move to
   end, that is the radio; note how often.
6. OTA per step 14 above (the corrected command), with the periscope idle.

## Results 2026-09-27, second run (fix build 60024f5, over-WiFi update)

OTA over WiFi worked (`espota.py` direct, `Result: OK`, board rebooted into the new build).
`late 0` at boot. `NO LINK` now appears. Commands from the page **reach the board during
lifter moves** (eight `SEQ replaced` / `QUEUED=SEEK` lines mid-travel) but the running move
finished before the new one started: the sequencer replaced the pending chain and left the
running job alone. E-STOP: the board received two E-STOP requests in the whole 321 s session
(`hits=2`, one latch at 73 s while idle). Taps on the other buttons during moves produced
requests; E-STOP taps during moves produced none, not even at the separate port, so those
presses were dying inside the page on the phone, cause unknown from the log.

Fix build: a new lifter or rotary command now **interrupts** a running seek or rotation (a
home, a characterisation or a calibration still finishes); the E-STOP button fires through four
independent channels (port 81 by fetch and by image request, the web server by fetch and by
sendBeacon), on the touch itself as well as the click, and shows a send counter; the board
prints `ESTOP PORT from <ip>: latch` / `ESTOP WEB from <ip>` for every arrival; the version line
carries the firmware hash instead of a compile stamp that the build cache can leave stale.

## Re-test for this build (short)

1. Boot line reads `phase 3  build <8 hex digits>`; `#PSTATUS` shows the same digits and the
   `WIFI=` line ends `estopPort=81 hits=0 webEstops=0 stalledDropped=0`.
2. Tap **Up**, and while the mast is rising tap **E-STOP**. Watch the button: it must change to
   `SENDING E-STOP… (1)` at once and the number must climb about three times a second. Tell me
   the number it reached and what it said at the end (`E-STOP LATCHED`, or
   `NO ANSWER: USE THE POWER SWITCH`). On the monitor, `ESTOP PORT from ...` / `ESTOP WEB
   from ...` lines show what arrived and when. Repeat three times.
   - Button never changes: the tap is not reaching the page's code; say so, that is the
     answer I need.
   - Button counts but no `ESTOP ...` line on the monitor until the move ends: the phone's
     requests are not leaving or not arriving while the motor runs (radio).
   - `ESTOP PORT ... latch` mid-move and the motor stops: fixed.
3. Tap **Up**, and while rising tap **Down**: the rise must stop and the descent start at
   once (`SEEK ABORTED CANCEL` then `SAFE_LOWER` on the monitor). Then Up, and while rising tap
   **50%**: same, it retargets. Home (`:PA0`) is deliberately not interruptible.
4. `#PSTATUS` afterwards: `hits=`, `webEstops=`, `stalledDropped=`.
