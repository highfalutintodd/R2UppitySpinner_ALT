# Bench procedure — Phase 1 (motion core + USB console)

For Todd. This is the first time v4 moves anything. Everything below is written to be run one
line at a time from a serial monitor at 115200 baud, with a hand on the power switch.

## Before power

- **Dome off. Head clear of everything. Nothing near the mast.** Phase 1 has no calibration
  memory and no commissioning lock survives a reboot, so treat every boot as a fresh board.
- Flash over USB (the partition layout changes, so OTA from v3 is not possible):
  ```
  arduino-cli compile --profile uppity UppityFirmware
  arduino-cli upload  --profile uppity -p /dev/cu.usbserial-XXXX UppityFirmware
  ```
  or open `UppityFirmware/UppityFirmware.ino` in Arduino IDE 2.3+, pick the `uppity` profile
  next to the board selector, and upload. Do not choose a partition scheme by hand.
- Serial monitor at 115200, line ending CR or LF (either works). `:PX` from the monitor is a
  real E-STOP that lands within a millisecond of the last byte arriving; keep it typed and
  ready to send, or keep the power switch under your hand. Both are fine.

## What is different from v3, in one breath

E-STOP latches (`:PX` stops everything and the board stays STOPPED until `#PRESUME`); the board
boots LOCKED and needs `#PUNLOCK` every boot in this phase; `:PH` no longer loops if the home
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
   t=0.xxx BOOT lifter=KNOWN rotary=UNKNOWN limits=[top=0 bot=1 home=0]
   ```
   If `EXPANDER=NOT RESPONDING`, stop: the PCF8574 is not answering at 0x20 and motors cannot be
   enabled at all. If `HAL=FAILED ...`, stop and send me the message.
2. `#PSTATE` → `lifter=0/450(0%) lifterConf=KNOWN`, `LIMITS=top:0 bot:1 home:0`.
   - If `bot:0` with the mast physically down, or `top:1`, the limit polarity is the other way
     round from the default (normally-open to ground). **Stop here** and tell me which switch
     reads what; Phase 2 adds the polarity setting.
3. Power off, raise the mast by hand is not possible on this mechanism, so skip the mast-up boot
   unless it happens to be up. If it is up: expect `limits=[top=1 ...]` and `lifter=450/450`.
4. Press the home switch plunger with a fingertip and watch `#PSTATE`: `home:1` while pressed.
   Release: `home:0`. If pressing it shows `ROTARY UNKNOWN -> SWITCH_LIVE` in the log, that is
   correct: a closed switch at rest is trusted as a zero.
5. `#PSTATE` once more and note `tick=[max Nus late M]`. Expect `max` well under 200 µs and
   `late 0`. Send me the numbers.

## First motion — lifter only

6. `#PUNLOCK`. Expect `COMMISSIONED=1` and `STATE LOCKED -> IDLE`.
7. `:PP20` (seek to 20 %). Expect `SEEK START`, phases, then `SEEK DONE_TARGET TARGET`.
   - `SEEK FAILED ENCODER_DIRECTION` means the encoder counts the wrong way for the drive: the
     mechanism is "inverted" in v3's terms. **Stop** and tell me; Phase 2 adds the invert setting.
   - `SEEK STALLED BREAKAWAY` or `STALL` means it did not move at 65 %. Stop and tell me.
   - Listen to the motor: 10 kHz PWM may whine faintly. Note whether it is objectionable.
8. `:PP100`. Expect `SEEK DONE_LIMIT LIMIT` and `LIMIT TOP made`. **Watch and listen to the
   arrival at the top switch**: it should be a gentle stop, not a slam. `#PSTATE` → `450/450`.
9. `:PP0`. Same at the bottom. Then `:PP50` and `#PSTATE` → about 225.
10. `:PP100`, and while it is moving send `:PX`. Expect `ESTOP LATCHED (source 1)`,
    `STATE MOVING -> STOPPED`, and the motor stopping at once. `:PP0` now → `SEEK REFUSED ESTOP`.
    `#PRESUME` → `ESTOP=released`, `STATE STOPPED -> IDLE`. Nothing moves on its own.

## First motion — rotary

11. `:PP100` (the rotary only turns with the head clear of the dome; with the dome off this is
    still the rule the firmware enforces). Then `:PA0` → a home search. Expect
    `HOME START`, `HOME phase ...`, `HOME EDGE enter=... exit=... closures=...`,
    `ROTARY UNKNOWN -> LATCHED`, `HOME DONE`.
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

## The whole thing

15. From raised: `:PH` → `SAFE_LOWER` runs `HOME` (if needed) then the descent; expect
    `LIMIT BOTTOM made` at the end. From the bottom: `:PH` → `SAFE_LOWER DONE ALREADY_THERE`.
16. Disconnect the home switch (or hold the plunger away), raise with `:PP100`, then `:PH`.
    Expect `SAFE_LOWER FAILED HOME_FAILED` and the mast **staying up**. Reconnect, `:PA0`, `:PH`.
17. Optional, long, keep clear: `#PSC` (calibration). It creeps up and down four times, probes
    the minimum power from each end, then runs a series of short pulsed moves at each 5 % step.
    Expect `CAL travel=... spread=...` and `CAL min power up=... down=...` at the end. Send me
    the lines; nothing is stored yet (Phase 2).

## Send back

The full serial log of the session, and: the `tick=[...]` numbers, whether the PWM whine is
acceptable, the arrival behaviour at both limits, and the `ROTARY ticks/rev` / `contact arc`
lines. If anything moved in a way you did not expect, the log line right before it is the one I
need.
