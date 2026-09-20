# Bench procedure — Phase 2 (settings that survive a power cycle + the command layer)

For Todd. Phase 2 adds what Phase 1 forgot at every boot: settings now live in flash, v3's
numbers are read across once, and the serial side is complete (command UART, stored sequences,
chains with waits, random mode, light kit, trigger inputs). Nothing in the motion core changed,
so the Phase 1 moves behave exactly as they did on 2026-09-19/20.

Everything below is written to be run one line at a time from a serial monitor at 115200 baud,
with a hand on the power switch. Head clear of everything. `:PX` is still the E-STOP and still
lands within a millisecond even mid-chain; `#PRESUME` releases it.

## Flash (USB, same as Phase 1)

Open the Terminal app and paste these three lines **one at a time**, pressing Return after
each. Close any serial monitor first: only one program can hold the port.
```
cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
arduino-cli compile --profile uppity UppityFirmware
arduino-cli upload  --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
```
Success looks like `Sketch uses 407563 bytes (20%)` (the number may differ by a little) after the
compile, then `Hash of data verified` and `Hard resetting via RTS pin` after the upload.
Then open the monitor (it also logs to a file on the Desktop, which is what "send back the log"
means):
```
arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase2-log.txt
```
Type a command such as `#PSTATE` and press Return to send it. Ctrl+C leaves the monitor.

## The one-time setup, in four lines

Phase 1 needed `#PINVERT1 #PTRAVEL1150 #PSAFESPIN904 #PROTARYTPR11855 #PUNLOCK` after every boot.
Phase 2 reads `904` (safe-spin) out of v3's flash by itself on its first boot. The other four are
sent **once** and stay:
```
#PINVERT1
#PTRAVEL1150
#PROTARYTPR11855
#PUNLOCK
```
Each answers with its own line ending in `(saved)`, and a `PERSIST=saved settings` line follows
within a second. Defaults that already match your choices and need nothing: `#PHOMEPARK1`,
`#PWIGGLE2`, `#PHOMEOFFSET0`, `#PMOTOR1` (19:1).

## What is different from Phase 1, in one breath

Settings persist and `#PCONFIG` prints them as lines you can paste back. The board boots
unlocked once `#PUNLOCK` has been saved (`#PLOCK` undoes it). The first boot of v4 imports v3's
stored tuning, conservatively: a value is taken only if it is clearly yours, never if it is a v3
default (v3's default safe-spin of half travel is what hit the lifter frame). The command UART
on GPIO16 is live (9600 baud, `#PBAUD<n>` changes it) with its own line buffer, so Sabé's and
Roam-A-Dome's heartbeats can never splice into a typed command. Stored sequences work again
past their first element (`#PS3:P100:L5:R30` is stored as `:PP100:PL5:PR30` and `:PS3` replays all
of it). `:PW` waits no longer freeze the board: the web page and E-STOP stay live, and `#PSTATE`
answers during a wait. `:PM` random mode runs as a script through the same safety policy as
typed commands. Flash writes never happen while a motor is enabled: they are deferred and
logged, and land as soon as the board is idle.

## Boot checks, first boot after flashing (no motion)

1. Power on with the mast **down**. Expect, in order:
   ```
   Uppity Spinner ALT v4.0.0-dev  phase 2 (...)
   HAL=ok
   EXPANDER=ok (drivers off, lights off)
   SETTINGS=none (first boot of v4)
   V3 import: motor profile 1 (Pololu 4751 (19:1, high-torque))
   V3 import: lifter min power 50% (up and down)
   V3 import: approach power 30% is the profile default, skipped
   V3 import: rotary min power 40% is the default, skipped
   V3 import: travel 1350 is the profile guess (v3 never measured it), skipped: send #PTRAVEL<ticks> or run #PSC
   V3 import: safe-spin height 904 ticks above the bottom switch (v3 minheight); confirm by eye before the first rotation below the top
   V3 import: rotary 1191 ticks/rev (v3's figure; #PROTARYTEST3 or #PROTARYTPR<tenths> to refine)
   V3 import: wizard state 7 noted; v4 stays LOCKED until #PUNLOCK (or commissioning, phase 4)
   V3 import: 4 values taken
   CONFIG motor=1(Pololu 4751 (19:1, high-torque)) inverted=0 travel=1350 (profile guess) safespin=904 tpr=1191.0 homepark=1 wiggle=2 offset=0 commissioned=0 id=0 baud=9600
   CONTROL_TASK=running core1 prio10 1kHz
   READY (type #PHELP)
   t=0.xxx BOOT lifter=KNOWN rotary=SWITCH_LIVE limits=[top=0 bot=1 home=1]
   PERSIST=saved settings
   ```
   The import lines are the exact reading of your v3 flash from 2026-09-20. If a line differs,
   send me the log; nothing moves at boot regardless. The `50%` lifter min power is your v3
   tuning and now floors the slowest seek at 50 % (Phase 1 used the profile's 45 %); if the
   slow seeks feel different, `#PMINUP0` and `#PMINDOWN0` go back to the profile/calibration.
2. Send the four one-time lines above. Expect for each, e.g.:
   ```
   #PINVERT=1 (saved) (re-referenced from the limit switches)
   PERSIST=saved settings
   ```
   `#PROTARYTPR11855` answers `#PROTARYTPR=11855 (saved) (marked confident: ...)`. `#PUNLOCK`
   answers `COMMISSIONED=1 (saved: the board boots unlocked from now on; #PLOCK to undo)` and
   the log shows `STATE LOCKED -> IDLE`.
3. `#PCONFIG`. Expect `CONFIG=begin ...`, one `#PKEY=VALUE` line per setting, `SEQ=none stored`,
   `CONFIG=end travel_in_use=1150 (#PTRAVEL)`. Check `#PINVERT=1`, `#PTRAVEL=1150`,
   `#PSAFESPIN=904`, `#PROTARYTPR=11855`, `#PCOMMISSIONED=1`, `#PMOTOR=1`, `#PHOMEPARK=1`,
   `#PWIGGLE=2`. Every one of those lines can be pasted back later to restore a setting.
4. **Power off, wait five seconds, power on.** This is the test that matters. Expect
   `SETTINGS=loaded schema 1`, **no** `V3 import` lines, and
   `CONFIG motor=1(...) inverted=1 travel=1150 (#PTRAVEL) safespin=904 tpr=1185.5 homepark=1 wiggle=2 offset=0 commissioned=1 id=0 baud=9600`.
   `#PSTATE` → `STATE=IDLE lifter=0/1150(0%) lifterConf=KNOWN ... tpr=1185.5`. Nothing to re-send.
5. `#PSTATUS` → version, uptime, `commissioned=1 random=off seq=idle persist=saved saves=N`,
   `SERIAL=usb+cmd@9600 id=0 dedup=750ms ...`, `EXPANDER=ok`.

## Persistence while moving (the deferral rule)

6. `:PP50` and, while it is still moving, `#PWIGGLE3`. Expect
   `#PWIGGLE=3 (saved)` at once, then `PERSIST=deferred (motors armed); it will save when the
   board is idle`, and about two seconds after `SEEK DONE_TARGET` (the drivers stay enabled for
   two seconds after a move) `PERSIST=saved settings`. Then `#PWIGGLE2` to put it back.
   Nothing is written to flash while a motor is enabled, ever; a request just waits.

## Chains and waits

7. `:PP100:PW2:PP0` (the README example that v3.5.2 broke). Expect `SEQ start 3 elements`,
   `QUEUED=SEEK id=N (step 1/3)`, the raise, a two-second pause, `QUEUED=SEEK ... (step 3/3)`,
   the descent (it ends on the bottom switch), `SEQ done`. During the pause send `#PSTATE`: it
   answers immediately, which v3 could not do inside a wait.
8. `:PL7:PP100` then `:PP0:PL5` (the two v3.5.3 chain bugs). Both halves of each must run:
   `LIGHT=7` and the raise; the descent and `LIGHT=5`. `:PL1` afterwards (lights off).
9. `:PW600:PL2` → `WAIT clamped from 600s to 60s`; the light changes after a minute. `:PX` at
   any point clears a running chain: `SEQ cleared at step ... (ESTOP)`; `#PRESUME`.

## Stored sequences

10. Store the README's set (each answers `Sequence [n]: <canonical form>` then `SEQ n stored`):
    ```
    #PS1:H
    #PS2:P100
    #PS3:P100:L5:R30
    #PS6:A0
    #PS7:P100:L5:R-30
    #PS8:H:P50:W2:P85,35:A90,25:W2:A270,20,100:W2:P100,100:L5:R50:W4:H
    ```
    `#PL` lists them as `#PS1::PH`-style lines: `#PS3:PP100:PL5:PR30` and so on (the stored
    form always carries the full `:P` on every element, which is what makes them replay).
    A typo is refused whole: `#PS9:P100:Q5` → `INVALID=sequence element 'Q5' (nothing stored)`.
11. `:PS3` → up, searchlight, and a continuous spin at 30 %; the chain reports `SEQ done` while
    the head is still turning (a spin has no end). `:PR0` homes it. `:PL1`.
12. `:PS8`, the "sneaky periscope". It lowers to 50 %, waits, rises to 85 %, turns to 90° and
    270° with pauses, goes to the top, lights, spins at 50 % for four seconds, then `:PH`
    pre-empts the spin, homes and lowers. The two rotations happen at 85 % = 977 ticks, above
    the 920 ticks (`:PP80`) you confirmed by eye on 2026-09-20 and above the 904 stored as
    safe-spin, so the head is further out of the dome than in that check. Watch the first one
    anyway with `:PX` ready; if it looks close, stop and tell me and I will raise the sequence
    to the top before any turn.
13. `#PD3` → `SEQ 3 deleted`. `#PSEQNAME8=Sneaky` → `SEQ 8 name stored`; `#PL` shows
    `#PSEQNAME8=Sneaky` under sequence 8. **Power cycle**; `#PL` still lists them all.

## Random mode

14. Mast down, head at home. `:PMG` (gentle). Expect `RANDOM=on level=0 (gentle)` and a
    `RANDOM action=N next=...ms` line before each action (gentle only uses actions 1, 3, 4: big
    lift, random angle, look around). Let it run for about three minutes with `:PX` under your
    hand. What to look for: every turn of the head happens at the top (the director raises
    first), every descent below the safe height homes first (`HOME ...` before `SEEK` when the
    head was turned), and the LED pulses blue on its idle colour while random mode is on.
    `:PH` ends it: `RANDOM=off (command)` and the periscope homes and lowers.
15. `:PM` (medium, your stored `#PAGGRESSION=1`) for a few minutes: all six actions, including
    "pop up/down", "spin at top" (a spin for 2–5 s, then a stop) and "bob and weave". `:PL3`
    and `:PW1` during random mode do **not** end it (`RANDOM=on` stays in `#PSTATE`); any other
    `:P` motion command does. Optional: `:PMA` is aggressive (fast lifts capped at the 19:1
    profile's 75 %, rotary 70–100 %); run it only if you are happy with the head's play after
    the rebuild. `:PX` during random mode: `RANDOM=off (ESTOP)`, `SEQ cleared`, the usual
    `ESTOP LATCHED`; `#PRESUME` and nothing resumes on its own.

## Command UART (Marcduino bus, GPIO16)

16. Needs Sabé (or whatever drives the bus) connected. v4 opens the port at 9600 unless
    `#PBAUD` was changed; v3 kept its baud in EEPROM, which never persisted, so it was 9600
    there too unless you set it every boot. Send `:PP100` from Sabé: the USB console shows
    `QUEUED=SEEK id=N` and the periscope rises; Sabé's `&SABE,HB,...` heartbeats produce no
    output at all. The same line arriving on both USB and the bus inside 750 ms runs once
    (`#PSTATUS` counts `duplicates=`). A `:PX` from the bus latches exactly like one typed on
    USB (`ESTOP LATCHED (source 2)`). Nothing is transmitted back on the bus unless you set
    `#PCMDTX1` (that drives GPIO0, which is also the boot strap, so it is off by default).

## Trigger inputs (optional, needs a jumper wire)

17. With the periscope idle, ground input **C** on the expander header. Expect `TRIGGER=1` and
    sequence 1 (`:PH`) to play; `SEQ 1: 1 element`. Remove it; ground **B** → `TRIGGER=2` →
    sequence 2 (up). Grounding A gives 4, combinations 3–7, as on v3. A jumper already in place
    at power-up does nothing until it is removed and put back.

## Light kit

18. `:PL5`, `:PL0` (full cycle; the firmware sends Off first for 20 ms so a kit already cycling
    restarts), `:PLR` (random, never Off), `:PL1`. `:PH` still turns the lights off before
    lowering, as in Phase 1.

## Calibration (optional, ~6 minutes, keep clear)

19. `#PSC`. As in Phase 1 it homes the rotary first, creeps, probes and sweeps. New: at the end
    `CAL stored: travel=1181 minUp=25% minDown=15% (profile 1)` and `PERSIST=saved calibration`,
    plus `CAL note: #PTRAVEL=1150 still overrides the calibrated 1181; send #PTRAVEL0 to use the
    calibrated figure`. Leave `#PTRAVEL1150` as it is (full-travel seeks end on the switch either
    way; 1150 is where the switch closes, 1181 is the rest position after the creep's brake).
    `#PCAL` prints the stored table. **Power cycle**: `CALIBRATION=loaded travel=1181 ...` at boot.

## Housekeeping

- `#PRESTART` reboots (refused while something moves). `#PFACTORY` clears every v4 setting,
  the calibration and all sequences and reboots; on your board the next boot would import v3's
  numbers again and you would re-send the four one-time lines. Not needed for this bench.
- `#PV3` prints v3's stored values; `#PV3IMPORT` runs the import again (same rules).
- `#PNCL` / `#PNOL` (lifter limits normally closed / open) and `#PNCR` / `#PNOR` (rotary home)
  are saved and take effect after `#PRESTART`. Yours are normally open, the default.
- `#PID<n>`: Marcduino id. `:P3P100` with id 3 set runs; with any other id it is dropped
  silently. Id 0 (default) accepts id-less lines, which is what Sabé sends.

## Send back

The log file from the Desktop, and in particular: the `V3 import` block from the first boot,
the `CONFIG ...` line from the boot after the power cycle (step 4), whether step 6's
`PERSIST=deferred` then `PERSIST=saved` came in that order, the `:PS8` run (step 12), and a few
minutes of `:PMG` (step 14). If anything moved in a way you did not expect, the log line right
before it is the one I need.
