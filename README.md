# R2 Uppity Spinner ALT

**Firmware for the Uppity Spinner periscope board in an R2-D2 dome: the lifter that raises the
periscope and the rotary that turns it.**

**Version 4 is a clean-sheet rewrite.** It is written from scratch and no longer uses the
ReelTwo library or any of the original code. It runs on the same board with the same wiring, and
every `:P…` and `#P…` serial command from v3 still works, so your Marcduino sequences, Sabé
buttons, WCB stored commands and DroidNet buttons carry over.

> **4.0.0-beta.1 is a beta.** It has been run end to end on one droid: Todd's, with a 19:1
> lifter, Sabé and three WCBs. It has not yet run on a 6.3:1 or IA-Parts lifter, or on anyone
> else's rotary. If you test it, please report what happened (see
> [Reporting a problem](#reporting-a-problem)). For an event where you cannot afford surprises,
> v3.6.0 is still the stable release, and you can go back to it at any time.

---

## What's in v4

- **Safety built in, in one place.** The head is never lowered into the dome unless it is turned
  home, and never turns below the height at which your head clears your dome. If the board does
  not know where the mast is, it allows only upward moves until a limit switch tells it. Nothing
  (a script, a stored sequence, the mesh, random mode) gets around these rules.
- **A real E-STOP.** `:PX`, the page's E-STOP button, Sabé or the WCB mesh stop both motors at
  once, even mid-sequence. The mast is braked to a stop rather than left to coast, and nothing
  moves again until you release it on purpose.
- **Commissioning measures your periscope.** A guided, seven-step setup measures your lifter's
  travel, your rotary's ticks per turn, how your lifter brakes, and, with you watching, the
  height at which your head clears your dome. Nothing is taken from a profile guess.
- **A completely new web interface.** Four pages (Operate, Setup, Diagnostics and Rescue) that
  work on a phone, with firmware updates from the page, live switches and encoders, the board's
  log, and light or dark themes. Rescue has a 60-second safety override, hold-to-jog for each
  motor, measured nudges and "set home here".
- **WCB mesh membership (optional).** The periscope can join Greg Hulette's WCB network as a
  board of its own: it takes moves, E-STOPs and status questions by board number, and sends its
  height, angle, state and faults as `&PERI` messages that Sabé and DroidNet can show.
- **More precise motion.** Every move brakes by the speed it actually reached, using your own
  calibration, so short moves move and every move lands within a few ticks of where it was sent.
  Homing is rebuilt around how the home switch really behaves, and learns your rotary's real
  ticks per turn.
- **The long-standing v3 problems are fixed**, among them:
  - two parts of the chip that could both drive the motors at once;
  - a web page that froze during moves, and an E-STOP the setup wizard could make unreachable;
  - the rescue override that was cancelled a moment after you pressed it;
  - four different ideas of where "home" was, and a failed home that re-zeroed the encoder;
  - homing that looped for a minute when it could not find the switch;
  - an E-STOP that a queued sequence step could undo;
  - stored sequences that broke after their first step;
  - `:PD` stopping at 180° and ignoring its speed;
  - the lifter count starting at zero at power-up wherever the mast was;
  - calibration that may never have been saved.
- **Tested before it reaches the droid.** A simulated periscope runs the real control code on a
  computer (235 tests), and every fault found on the bench became one of those tests.

## What stays the same

- The board and the wiring. No hardware changes. (The SD-card board variant is not supported;
  stay on v3 for that one.)
- Every `:P…` and `#P…` command and chaining with `:`, the board ID prefix, the light kit modes,
  random mode (gentle, medium, aggressive), the three lifter motor profiles, expert mode for the
  19:1, the trigger inputs, the status LED.
- The board's own WiFi network and the web page at `http://192.168.4.1`.

The full command list is in [docs/COMMANDS.md](docs/COMMANDS.md).

---

## Hardware

- **Board:** the Uppity Spinner PCB with an ESP32-WROOM-32 (standard variant, not the SD-card one).
- **Motor drivers:** two TB9051FTG, one for the lifter and one for the rotary.
- **Lifter motor profiles** (`#PMOTOR`, or step 1 of commissioning):

  | Profile | Typical travel | Notes |
  |---|---|---|
  | Pololu 4757, 6.3:1 | ~450 ticks | the original fast lifter |
  | Pololu 4751, 19:1 | ~1,350 ticks | more torque for stiff lead screws; gentle at the top of travel |
  | IA-Parts | ~845 ticks | |

  The profile only sets starting values. Commissioning measures your lifter's real numbers.
- **Rotary:** any of the usual motors. Builders' rotaries range from about 250 to 1,200 encoder
  ticks per turn; commissioning measures yours.
- **Recommended motors (Pololu):** 12 V [#4841](https://www.pololu.com/product/4841) lifter and
  [#4847](https://www.pololu.com/product/4847) rotary; 6 V [#4801](https://www.pololu.com/product/4801)
  and [#4807](https://www.pololu.com/product/4807).

For assembling the board from scratch, the original videos are still the best reference:
[Part 1](https://www.youtube.com/watch?v=x4_3irdV4C8), [Part 2](https://www.youtube.com/watch?v=MdSRJsYx1T8).

---

## Installing

Download the release from the
[Releases page](https://github.com/highfalutintodd/R2UppitySpinner_ALT/releases). The download
has three ways in; `FLASHING.txt` inside it has the exact commands. Pick one:

| Your board runs… | Use | Keeps settings? |
|---|---|---|
| v4 already | `…-update.bin`, from the page: **Setup → Firmware → Choose File → Upload** | yes |
| v3 (3.x), and you want v4 to copy its settings | the four files in `upgrade-from-v3/`, over USB, each at the address in its name | v3's settings are copied once, then commissioning |
| nothing yet, or you want a clean start | `…-full.bin`, over USB, at address `0x0` | no: everything is erased |

**USB flashing without installing anything:** Espressif's own page,
[espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/), works in Chrome or
Edge. Connect, pick the board's port, add each file at its address, and press **Program**.
(Command-line users: `esptool.py`, with the commands in `FLASHING.txt`.)

**Why the first install from v3 needs USB:** v4 is bigger than v3 and needs a different split of
the board's flash. After that first USB install, every update goes through the page.

**What v4 takes from v3:** on its first boot, v4 reads v3's saved settings once and keeps the
ones it trusts (motor profile, minimum powers, a measured travel, a safe-spin height you set,
the rotary's ticks per turn, drift, aggressiveness, expert mode). It prints a `V3 import: …`
line for each, including the ones it skipped and why. It does **not** copy v3's stored
sequences: list them on v3 first (`#PL`) and type them back in on v4.

**Building it yourself:** the sketch is `UppityFirmware/`, pinned to the ESP32 Arduino core
3.3.4 by `UppityFirmware/sketch.yaml`. In Arduino IDE 2.3 or later, pick the **uppity** profile.
From the command line, in the repository folder:

```
arduino-cli compile --profile uppity UppityFirmware
arduino-cli upload --profile uppity -p <your port> UppityFirmware
```

No extra libraries to install: the one v4 needs (WCB_Client, for the mesh) is in the sketch
folder. `tools/release.sh` builds the same three sets of files the Releases page carries.

(The v3 sketch, its `libraries/` and `menus/` folders are still in the repository root during
the beta, for reference. They are not used by v4.)

---

## First-time setup

1. **Join the board's WiFi.** Its name is `R2Uppity-` followed by four characters from the
   board's own address, for example `R2Uppity-1390`. The password is `Astromech`. Open
   `http://192.168.4.1` (most phones open it by themselves).
2. **Change that password.** Everyone who has read a README knows `Astromech`, so anyone near
   your droid could join and drive the periscope. The page shows a warning banner until you do:
   **Setup → WiFi**, the board's own network password (8 to 32 characters), then **Restart to
   apply**, and join again with the new password. Set it
   on the page, not in a serial monitor: monitors that log keep every line you type.
3. **Commission the board** (**Setup → Commissioning → Start**). Until it is done, the board is
   LOCKED and neither motor moves. The checklist remembers where you were through a page reload
   or a power cycle, and an E-STOP pauses it. Seven steps:

   | Step | What happens |
   |---|---|
   | 1 Profile | pick the lifter motor profile and confirm the checklist |
   | 2 Wiring check | the page shows each switch and the motor drivers live; it passes when nothing contradicts itself |
   | 3 Lift test | the first motion: a slow creep **up** to the top switch |
   | 4 Rotary | at the top, three slow turns to measure ticks per turn, then a home |
   | 5 Calibration | bottom, top, bottom, top, bottom, measuring travel and how the lifter brakes |
   | 6 Safe-spin height | the head turns 90° at the top; you lower it in small steps, watching, until it is just above where it would touch; the board stores that height plus a margin |
   | 7 Acceptance | home, 0 %, 50 %, 100 %, home, and one gentle random action |

   **Step 6 is the one that matters most.** It is the height above which the head may turn.
   Do it with the dome on, the way the droid will run. Lower in small steps, and stop as soon as
   anything looks close. Until it is set, the board treats only the very top as clear.

   There is one bypass, at the bottom of the commissioning card, for an emergency at an event
   ("Unlock without commissioning"). The motors then run on the profile's guesses, and only the
   top counts as clear for turning the head.

4. **Optional: join your home WiFi** (**Setup → WiFi**). The board's own network stays up too, and
   the page is then also at `http://uppity.local` or the address the board prints at boot.

---

## Everyday use

**The web page** has four parts:

- **Operate:** up, down and the height presets; home and the rotary presets; random mode
  (gentle, medium, aggressive), Stop, and the light kit modes.
- **Setup:** commissioning, motor and calibration, tuning, the serial port, WiFi, the mesh,
  firmware upload, and the light or dark theme.
- **Diagnostics:** the switches and encoders live, the rotary test, the board's log, and the full
  configuration in a form you can paste back.
- **Rescue:** for a stuck periscope. A 60-second safety override, hold-to-jog for each motor, small
  measured nudges, and "set home here".

The **E-STOP** button is at the top of every page. After an E-STOP, the page shows a red banner
and nothing moves until you press **Reset** there (or send `#PRESUME` over serial, or `&ESTOP,RESET`
over the mesh). `:PX`, `?STOP`, `&ESTOP` and `&SABE,ESTOP` latch it from any serial port, even in
the middle of a chain.

**Serial control** is unchanged from v3: the command port runs at 9600 baud by default, the USB
port at 115200. Lines starting `:P` move, lines starting `#P` configure or ask, and lines for other
boards are ignored without a word. See [docs/COMMANDS.md](docs/COMMANDS.md).

**Trigger inputs:** the three inputs, read as a number from 1 to 7, play the stored sequence with
that number (`:PS1` … `:PS7`).

**Updating:** **Setup → Firmware → Choose File** with the `…-update.bin` from a release, then
**Upload**. The page refuses while a motor is running. (The board also answers `espota.py`, with
the board's own network password as the update password.)

---

## The WCB mesh (optional, off by default)

If your droid uses Greg Hulette's [Wireless Communication Boards](https://github.com/greghulette/Wireless_Communication_Board-WCB),
the periscope can join their ESP-NOW network as a board of its own (**Setup → Mesh**). Tested with
WCB firmware 7.0.1; 6.2.0 or newer is required.

**What it gives you:** the periscope sends `&PERI` heartbeats (state, height, angle, head home)
every 10 seconds to every board, position updates and fault reports to your controller (board 20,
for example Sabé), and it takes `:P` moves, E-STOPs and status questions sent to its board number.
A board numbered above the WCBs' quantity works on WCB 7.0.1: the WCBs learn it from its
announcements.

**What it costs:** while the mesh is on, the board does not join your home WiFi. The page is then
only on the board's own network, at `http://192.168.4.1`. Turning the mesh off brings everything
back after a restart.

**What the mesh may not do:** change settings, run commissioning, unlock, restart, or use the
rescue override. Those come only from the USB port or the page. Every move from the mesh goes
through the same safety rules as everything else.

**Setting it up:** copy the values from a WCB's `?BACKUP` listing: the octets (its `?MAC,2` and
`?MAC,3` lines), the password (`?EPASS`), the quantity (`?WCBQ`) and the channel (`?WCBCH`).
**Do not type those commands on a WCB to look a value up: on WCB firmware they change it.** Pick
a board number nothing else uses, save, and restart. From a WCB's console, `;W5:PP100` then sends
`:PP100` to board 5. On WCB 7.0.1, a `?STOP` typed on a WCB stays on that WCB; use `;W5&ESTOP` to
stop the periscope.

---

## What changed from v3

- **E-STOP latches** until it is released (above). A controller button that sends `:PX` needs a
  second button that releases it, or the page's Reset.
- **Every board is commissioned once.** The motors are locked until then (or until the bypass).
- **No "safety maneuver" on the first command after power-up.** The board works out where the mast
  is from the limit switches. If it cannot tell, it allows only upward moves until it can.
- **`:PH` does not loop** raise, home, lower for a minute when the home switch is not found. It
  stops raised, says why, and waits.
- **A turned head stops at the safe-spin height** instead of going lower. A second descent is
  refused until the head is home.
- **Short moves move, and every move lands close to where it was sent.** The lifter brakes by the
  speed it actually reached, using its own calibration.
- **Dropped:** the SMQ Droid Remote and its OLED menus, RC/PPM input, the SD-card board variant,
  Marcduino over WiFi (TCP port 2000), and the patched ReelTwo library.
- **Not in this beta:** a sequence editor on the page. Stored sequences work as before, over
  serial (`#PS…`).

### Why a rewrite

v3 grew out of the original firmware and the ReelTwo library. Two parts of the chip could drive
the motors at once, long moves blocked everything else, and there were several ideas of where
"home" was. In September 2026 that cost a full day at an event, and each patch moved the problems
somewhere else. v4 starts again on one principle: one part of the firmware drives the motors,
anything can stop them, and the safety rules live in one place that nothing bypasses.

---

## Troubleshooting

**The page says LOCKED and nothing moves.** The board has not been commissioned (or its settings
were erased). Run **Setup → Commissioning**.

**Nothing moves and the page shows a red banner.** E-STOP is latched. Press **Reset**.

**It refuses to go down.** The head may be turned, or the board cannot confirm it is home: the
banner says which. **Home the head** (it rises first if it needs to), then lower.

**After power-up it only goes up.** The mast was somewhere between the switches, so the board does
not know its height. Send it up once; the top switch tells it.

**The page is not at 192.168.4.1.** Check that your phone joined the board's own network, not your
home WiFi. With the mesh on, the board is never on your home WiFi.

**The light kit cycles through every mode and ignores `:PL<n>`.** If you run the
[drolsen r2-periscope-lighting](https://github.com/drolsen/r2-periscope-lighting) sketch, set
`COMTYPE = 0` (ABC mode) in `PeriscopeLightsCode.ino`. The board drives the light kit over the
parallel A/B/C pins only.

**The head brushes the dome opening on the way down.** The board only lowers a head it has just homed, but "home" is where the home switch closes, and most rotary gearboxes allow a couple of degrees of slop. If your opening leaves very little room, centre home in it: with the periscope down, look at the gap on each side of the lens, then shift home a few ticks at a time with `#PHOMEOFFSET` (about 3 ticks per degree on a 1,185-tick rotary). After each change, home the head at the top and look before you lower it.

**Erratic readings with a metal dome.** Twist the encoder and switch wires and keep them away from
the dome. Good wiring is the foundation; the software's checks are a second layer.

---

## Reporting a problem

Open an [issue](https://github.com/highfalutintodd/R2UppitySpinner_ALT/issues) with:

- your lifter and rotary motors and mechanism, and what you did when it went wrong;
- the board's log (**Diagnostics → Log**, or a serial monitor at 115200 from power-up);
- the output of `#PSTATUS` and `#PCONFIG`. Passwords are never printed, so these are safe to share.

## Going back to v3

Flash v3.6.0 over USB the way you always did. v4 keeps its settings apart from v3's, so v3 finds
its own where it left them.

---

## Credits

Original firmware by [reeltwo / Skelmir](https://github.com/reeltwo/R2UppitySpinnerV3). This fork is
maintained by [highfalutintodd](https://github.com/highfalutintodd), with help from the R2 Builders
Club community. The WCB mesh client is Greg Hulette's
[WCBClient](https://github.com/greghulette/WCBClient) (MIT), included in `UppityFirmware/libs/`.
History of every release: [CHANGELOG.md](CHANGELOG.md).

## License

LGPL-2.1, see [LICENSE](LICENSE).
