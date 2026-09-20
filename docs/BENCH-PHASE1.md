# Bench procedure — Phase 1 (motion core + USB console)

For Todd. This is the first time v4 moves anything. Everything below is written to be run one
line at a time from a serial monitor at 115200 baud, with a hand on the power switch.

## Before power

- **Dome off. Head clear of everything. Nothing near the mast.** Phase 1 has no calibration
  memory and no commissioning lock survives a reboot, so treat every boot as a fresh board.
- Flash over USB (the partition layout changes, so OTA from v3 is not possible). Open the
  Terminal app and paste these three lines **one at a time**, pressing Return after each.
  The first line moves into the repo folder (the path has spaces, so keep the quotes). The
  last line needs the board's port name: plug the board in and run `ls /dev/cu.*` — on Todd's
  MacBook the Uppity Spinner shows up as `/dev/cu.wchusbserial110`.
  ```
  cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
  arduino-cli compile --profile uppity UppityFirmware
  arduino-cli upload  --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
  ```
  Success looks like `Sketch uses N bytes` after the compile, then `Hash of data verified`
  and `Hard resetting via RTS pin` after the upload.
  One-time setup on an Apple Silicon Mac: arduino-cli's `ctags` helper is Intel-only, so
  Rosetta 2 must be installed first or the compile fails with "bad CPU type in executable":
  ```
  softwareupdate --install-rosetta --agree-to-license
  ```
  Alternatively open `UppityFirmware/UppityFirmware.ino` in Arduino IDE 2.3+, pick the `uppity`
  profile next to the board selector, and upload. Do not choose a partition scheme by hand.
- Serial monitor at 115200, line ending CR or LF (either works). To open one, paste this in
  the same Terminal window (after the `cd` line above). It also saves everything to a log file
  on the Desktop, which is what "send back the serial log" means at the end:
  ```
  arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-log.txt
  ```
  Type a command such as `#PSTATE` and press Return to send it. Press Ctrl+C to leave the
  monitor. Only one program can hold the port: close the monitor before running `upload`.
  `:PX` from the monitor is a real E-STOP that lands within a millisecond of the last byte
  arriving; keep it typed and ready to send, or keep the power switch under your hand. Both
  are fine.

## Todd's hardware, as measured on 2026-09-19

Send these after every boot until Phase 2 saves them: `#PINVERT1` (mechanism runs backwards),
`#PTRAVEL1150` (top switch closes at ~1150 ticks; the 19:1 profile guesses 1350),
`#PSAFESPIN904` (v3's stored `minheight`, confirmed by eye at `:PP80` on 2026-09-20),
`#PROTARYTPR11855` (measured 1185.5 ticks/rev; without it the first `:PD`/`:PH` of a boot runs
the two-turn measurement). Defaults that already match Todd's choice and need no command:
`#PHOMEPARK1`, `#PWIGGLE2`, `#PHOMEOFFSET0`, motor profile 1 (19:1).
Rotary moves of 6° or less are walked one tick at a time, so `:PD2` moves about 7 ticks and
stops there; longer moves run continuously. The 19:1 profile is the boot default,
so `#PMOTOR` needs nothing.
v3's NVS, read with `#PV3` on 2026-09-20: `lftminpwr=50 lftseekbotpwr=30 rotminpwr=40
lftdist=1350 minheight=904 rotenccount=1191 driftpct=5 motortype=1 wizstate=7`. Notes:
`lftdist` is the 19:1 default, so v3 never measured travel (real ≈1181); `rotenccount=1191` is
the stale figure behind v3's rotary drift (real 1186); `minheight=904` is a tick height above
the bottom switch (77 % of real travel, shown as 67 % of 1350 on v3's page). If v3 had
re-referenced from the top when it was set, the physical height would be 735 instead; 904 is
the safe reading of the two. Confirm by eye before the first rotation below the top: with the
head at home, `:PP80` (920 ticks) and look at the gap between the head and the lifter frame. Rotary: 1185.6 ticks/rev, spread
0.1 %, contact arc ~50 ticks, one debounced closure per pass, drive sign as v3. Home switch
reads closed with the head parked at home (normal). Top switch releases ~45 ticks below the
top, bottom switch ~55 ticks above the bottom.

## What is different from v3, in one breath

E-STOP latches (`:PX` stops everything and the board stays STOPPED until `#PRESUME`); the board
boots LOCKED and needs `#PUNLOCK` every boot in this phase; settings such as `#PINVERT1` are
RAM only and must be sent again after every boot until Phase 2 adds persistence; `:PH` no longer loops if the home
switch is not found (it stays raised and reports `SAFE_LOWER FAILED HOME_FAILED`); no WiFi,
no stored sequences, no waits, no random mode, no command-serial input yet (USB only).

## Boot checks (no motion)

1. Power on with the mast **down**. Expect, in order:
   ```
   Uppity Spinner ALT v4.0.0-dev  phase 1 (...)
   HAL=ok
   EXPANDER=ok (drivers off, lights off)
   CONTROL_TASK=running core1 prio10 1kHz
   READY (type #PHELP)
   t=0.xxx BOOT lifter=KNOWN rotary=SWITCH_LIVE limits=[top=0 bot=1 home=1]
   ```
   `home=1` and `rotary=SWITCH_LIVE` are right when the head is parked at home (the ball
   bearing is sitting on the switch); with the head turned away expect `home=0` and
   `rotary=UNKNOWN`. Two `E (2xx) gpio: gpio_pullup_en(78)` lines before `HAL=ok` are
   harmless: the pulse-counter driver asks for pull-ups on the encoder pins 34/35, which
   cannot have them and do not need them.
   If `EXPANDER=NOT RESPONDING`, stop: the PCF8574 is not answering at 0x20 and motors cannot be
   enabled at all. If `HAL=FAILED ...`, stop and send me the message.
2. `#PSTATE` → `lifter=0/450(0%) lifterConf=KNOWN`, `LIMITS=top:0 bot:1 home:1` (home:1 only
   while the head is parked at home).
   - If `bot:0` with the mast physically down, or `top:1`, the limit polarity is the other way
     round from the default (normally-open to ground). **Stop here** and tell me which switch
     reads what; Phase 2 adds the polarity setting.
3. Power off, raise the mast by hand is not possible on this mechanism, so skip the mast-up boot
   unless it happens to be up. If it is up: expect `limits=[top=1 ...]` and `lifter=450/450`.
4. Press the **top lifter limit switch** with a fingertip for about a second and release it.
   (The rotary home switch cannot be reached on an assembled periscope; it is behind the pipe.)
   Expect `LIMIT TOP made IGNORED (bottom still made) at 0`, then `LIMIT TOP released at 0`.
   `#PSTATE` afterwards must still say `lifter=0/450(0%)`. The firmware refuses to believe a
   top edge while the bottom switch is still pressed; while both are pressed motion is
   blocked, and it unblocks by itself on release.
5. `#PSTATE` once more and note `tick=[max Nus late M]`. Expect `max` well under 200 µs and
   `late 0`. Send me the numbers.

## First motion — lifter only

6. `#PUNLOCK`. Expect `COMMISSIONED=1` and `STATE LOCKED -> IDLE`.
7. `:PP20` (seek to 20 %). Expect `SEEK START`, phases, then `SEEK DONE_TARGET TARGET`.
   - `SEEK FAILED ENCODER_DIRECTION` means the encoder counts the wrong way for the drive: the
     mechanism is "inverted" in v3's terms. **Stop** and tell me; Phase 2 adds the invert setting.
   - `SEEK STALLED BREAKAWAY` or `STALL` with **no** `LIMIT BOTTOM released` line before it
     means the motor ran the wrong way and pushed into the bottom stop (the count creeps up a
     few ticks of compliance, the mast never leaves the switch). That is v3's "invert"
     setting, which v4 does not know yet. Send `#PINVERT1` (RAM only; it re-references the
     position from the switches, so `#PSTATE` shows `lifter=0/450` again) and repeat `:PP20`.
     `#PCONFIG` shows the current value as `#PINVERT=`.
   - `SEEK STALLED` **with** a `LIMIT BOTTOM released` line means it lifted off the switch and
     then could not keep going at full power. Stop and tell me.
   - Listen to the motor: 10 kHz PWM may whine faintly. Note whether it is objectionable.
8. `:PP100`. Expect `SEEK DONE_LIMIT LIMIT` and `LIMIT TOP made at N`. **Look at N.** It is the
   count when the top switch closed, and it is the real travel of this lifter. If it is far
   from the profile's 450 (Todd's read ~1150), send `#PTRAVEL<N>` (RAM only, re-send after
   every boot) so that percentages, the safe-spin height and the hold logic use the real
   figure. `:PH`, `:PP0` and `:PP100` always run to the switch regardless. **Watch and listen to the
   arrival at the top switch**: it should be a gentle stop, not a slam. `#PSTATE` → `450/450`.
9. `:PP0`. Same at the bottom. Then `:PP50` and `#PSTATE` → about 225.
10. `:PP100`, and while it is moving send `:PX`. Expect `ESTOP LATCHED (source 1)`,
    `STATE MOVING -> STOPPED`, and the motor stopping at once. `:PP0` now → `SEEK REFUSED ESTOP`.
    `#PRESUME` → `ESTOP=released`, `STATE STOPPED -> IDLE`. Nothing moves on its own.

## First motion — rotary

11. `:PP100` (the rotary only turns with the head clear of the dome; with the dome off this is
    still the rule the firmware enforces). Then `:PA0` → a home search. Expect
    `HOME START`, `HOME phase ...`, `HOME DONE`, `HOME EDGE enter=... exit=... closures=...`,
    `HOME SETTLED on contact (wiggle N, nudges M)`, `ROTARY UNKNOWN -> LATCHED`.
    Phases: 3 sweep, 4 edge brake, 6 the settle wiggle (a decaying swing about the new zero,
    first swing 4°, last approach along the sweep direction so any play in the drive is taken
    up the same way every time; this is the visible "settling into place"), 7 the contact
    nudge (one tick at a time until the switch conducts at rest, then two ticks further in),
    8 the parking move to the visual home when `#PHOMEOFFSET` is set, 9 one continuous move
    to get clear of the arc on the approach side, 10 one continuous slow move back onto it
    that stops on the **first closure met while moving**, coast included, 11 done. Phases
    9–10 are the switch-only park (`#PHOMEPARK1`, the default). They are continuous moves,
    never pulses: on Todd's drive short pulses in the negative direction turn the count
    without turning the head (59 pulses, count -102, switch never seen), while continuous
    moves at floor power track it. This lands on v3's parking spot. v3 searched the other way round, so the
    default creeps in **against** the sweep and meets the far edge of v4's arc; `#PHOMEPARK2`
    creeps in with the sweep (near edge), `#PHOMEPARK0` parks at the arc centre by count.
    `#PHOMEOFFSET<N>` shifts the parking spot by N ticks if it still looks off. The log line is
    `HOME PARKED on the leading edge (wiggle N, park moves M, rest contact yes/no)`; "no" is normal
    on Todd's switch, whose contact only holds at rest deeper in the arc. Small `:PD` moves are
    only as honest as the count: on a slipping drive use `:PA` angles and the switch, not ticks. `HOME SETTLED NO CONTACT` means the head
    is within tolerance by the count but the switch would not conduct at rest: tell me the
    wiggle/nudge numbers.
    - `HOME FAILED NO_EDGE` after about 1.25 turns each way: the switch never closed while
      moving. Check the wiring; tell me `closures` from the log.
    - `HOME FAILED SWITCH_STUCK`: the switch reads closed for more than 60 ticks of travel:
      polarity or wiring.
12. `#PROTARYTEST3`. The head turns three-plus revolutions at low power. Expect
    `ROTARY ticks/rev=1184.x spread=0.x%` and `ROTARY contact arc=N ticks closures=M`.
    Compare with v3's 1184–1186 and its ~7 closures per pass. Send me the two lines.
13. `:PA90`, `:PA270`, `:PA0`. If `:PA90` turns the head the way `:PA270` should, the drive
    sign convention is the opposite of v3's: tell me and I flip `driveSign` (one constant).
14. `:PD720` → two full turns; `:PD-720` back. `:PR30` → spins; `:PR0` → homes.

## Rotary — extended (optional, same boot, mast may start down)

R1. From the bottom, `:PA90`. The head cannot turn inside the dome, so expect the lifter to
    raise to the top first (`SEEK START param=<travel>` … `LIMIT TOP made`), then
    `ROT_ABS START` … `ROT_ABS DONE_TARGET`.
R2. Homing repeatability: `:PA45`, `:PA0`, `:PA135`, `:PA0`, `:PA225`, `:PA0`, `:PA315`,
    `:PA0`. Each `:PA0` prints `HOME EDGE enter=A exit=B closures=1`. A and B should agree
    across the four homes to within a few ticks (2026-09-19: -25/10, -27/3, -24/13).
R3. Drift over many turns: `:PD3600` (ten turns), then `#PSTATE`: `rotary=N ticks` should be
    within about 12 ticks of 0 and `deg` near 0. `:PD-3600`, `#PSTATE` again. Then `:PA0`.
    A `HOME FRAME CORRECTED by N ticks` line anywhere here shows how far the count had
    wandered from the switch; on v3 this was ~18° per ten turns because its stored 1191
    ticks/rev was wrong. Expect small single digits or nothing.
R4. Speeds: `:PA180,30`, `:PA0,100`, `:PA180,30,60`, `:PA0`. Below the 40 % floor the speed is
    floored, so `,30` runs at 40 %. Watch for smooth starts and stops.
R5. E-STOP mid-spin: `:PR40`, wait two seconds, `:PX`. Expect `SPIN ABORTED ESTOP`,
    `ESTOP LATCHED (source 1)`, the head stopping at once. `:PA0` → `HOME REFUSED ESTOP`.
    `#PRESUME`, then `:PA0` homes. Then `:PR-30` for a few seconds (other direction), `:PR0`.
R6. Random: `:PAR` three times, `:PDR` three times, each `DONE_TARGET`; then `:PA0`.
R7. **Withdrawn.** As first written this step turned the head at 54 % of travel on the
    assumption that half travel clears the dome opening. On Todd's periscope it does not: the
    head hit the lifter frame (`ROT_ABS STALLED STALL` at 1968 s on 2026-09-19) and loosened.
    There is no safe guess for this height. The firmware now treats only the top as clear until
    `#PSAFESPIN<ticks>` is set; `#PV3` prints v3's stored `minheight` (and the rest of its
    tuning) straight out of NVS, which survived the reflash. Do not test rotation below the top
    until that value is in and has been confirmed by eye with the head turned 90° first at the
    top and the mast then lowered slowly toward the value with `:PX` ready.

## The whole thing

15. From raised: `:PH` → `SAFE_LOWER` runs `HOME` (if needed) then the descent; expect
    `LIMIT BOTTOM made` at the end. If the mast stops short with `SEEK DONE_TARGET` and no
    `LIMIT BOTTOM made`, you are on a build before 2026-09-19: the descent used to aim at
    count 0 instead of the switch. From the bottom: `:PH` → `SAFE_LOWER DONE ALREADY_THERE`.
16. This one needs the **rotary home switch** (the ball-bearing microswitch on the rotary
    stage, not a lifter limit switch) to be unreachable by the firmware. On an assembled
    periscope the switch itself is behind the pipe, so unplug its connector at the PCB instead
    (the wire that lands on GPIO 23). If that is not practical either, skip this step: the host
    test `ph_with_broken_switch_stays_raised_and_says_why` covers it. With it unplugged, raise
    with `:PP100`, then `:PH`. Expect `SAFE_LOWER FAILED HOME_FAILED` and the mast **staying
    up**. Reconnect, `:PA0`, `:PH`.
17. Optional, long, keep clear: `#PSC` (calibration). It creeps up and down four times, probes
    the minimum power from each end, then runs a series of short pulsed moves at each 5 % step.
    Expect `CAL travel=... spread=...` and `CAL min power up=... down=...` at the end. Send me
    the lines; nothing is stored yet (Phase 2).

## Results 2026-09-19 (Todd's periscope, 19:1)

Steps 1–15 and 17 passed on the third pass of the day; step 16 needs the rotary-switch
connector unplugged at the PCB and was not run. Fixes made along the way, each with a host
test: a limit edge while the opposite switch is made is ignored; `#PINVERT`, `#PTRAVEL`
bench commands; `:PH` ends on the bottom switch and homes after characterisation; "skip if
home" means within tolerance; `:PH` prints its own outcome; the 19:1 profile boots by default.
Calibration: `CAL travel=1181 ticks spread=1.3%`, `CAL min power up=250 down=150`
(25 % / 15 %). The travel figure is the rest position after the creep's brake, ~40 ticks past
where the switch closes; full-travel seeks end on the switch anyway. Tick health
`max 20us late 0` throughout. Rotary 1186 ticks/rev, arc 50–53, one closure per pass.
Rotary extended (R1–R6) all passed: homing edge -20…-27 / 13…19 across ten homes, ten
turns each way came back to within 5 ticks (0.4 %) of zero with no frame correction needed
(v3 lost ~18°), E-STOP mid-spin and both spin directions fine.
2026-09-20 result: with the continuous two-move park, three `:PA0` from 0°, 120° and 250°
all parked "spot on straight" (Todd), `rest contact yes` each time, return move met the switch
within 5 ticks of the same spot every time. Todd finds the pre-park wiggle a lot of movement
next to v3; `#PWIGGLE<deg>` (0–20, default 4) sets its first swing, 0 leaves only the park's
own out-and-back (~9°).
2026-09-20 (later): with `:PD-2` the count fell 8 per command in a uniform 377 ms while the
head stood still; `:PD2` took two to three times longer and did move it; the switch then closed
353 ticks from where the count said home was. Continuous moves still agree with the head. The
loose element behaves like a slipping clutch between motor shaft and encoder under short
pulses. Consequence: nothing tick-level may decide a home; the park pass now uses the switch
alone. Also: the rest-contact rule corrected the frame by 353 ticks the moment the switch
closed, which is the rule doing its job.
2026-09-20: the rotary drive has play at the motor shaft (from the frame strike). Homing
pass 3 rebuilt as a settle wiggle + one-tick contact nudge (both play-aware; the wiggle
measures its own coast on the first swing). Simulator gained a backlash model. To verify on
the reassembled head: ten `:PA0` from mixed angles should each end `HOME SETTLED on contact`
and the at-rest switch flicker should be gone.
Open: the home switch flickers at rest when the lifter thumps a limit (the bearing parks on
the edge of the contact zone); harmless, the trust label flips, but worth a settle-in after
homing in Phase 2.

## Send back

The full serial log of the session, and: the `tick=[...]` numbers, whether the PWM whine is
acceptable, the arrival behaviour at both limits, and the `ROTARY ticks/rev` / `contact arc`
lines. If anything moved in a way you did not expect, the log line right before it is the one I
need.
