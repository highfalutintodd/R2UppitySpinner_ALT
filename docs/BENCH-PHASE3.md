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
   SETTINGS=migrated from schema 2 (80 bytes) to 3     (first boot only; the WiFi fields were added)
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

14. From the laptop on the same network as the board (its own network, or the house WiFi once
    the board has joined), with the periscope idle:
    ```
    cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
    arduino-cli upload --profile uppity -p uppity.local --upload-field password=Astromech UppityFirmware
    ```
    (if you changed the AP password with `#PAPPASS`, use that instead of `Astromech`). The
    monitor shows `OTA 0% (motors held)`, `OTA 10%` ... `OTA 100% done, restarting`, and the
    board reboots into the new build. If a motor is enabled or a job is running the upload is
    simply not answered until the board is idle (the CLI reports a timeout: wait, then retry).
15. While a move is running, a `POST /update` from the page's API answers
    `update refused: a motor is enabled or a job is running`. (No button for it yet; Phase 4's
    Setup page gets one.)

## Send back

The log file, and: the boot `WIFI` lines, whether the phone joined and the page popped up by
itself, step 6 (E-STOP from the page while moving) and step 14 (OTA). If the page ever shows
`NO LINK` while the phone is still on the network, the `#PSTATUS` line at that moment.
