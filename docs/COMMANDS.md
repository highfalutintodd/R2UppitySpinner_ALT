# Uppity Spinner ALT 4.0 — serial commands

For firmware 4.0.0-beta.1 (the v4 rewrite). Everything here is taken from the firmware source:
`UppityFirmware/src/core/Parser.h` (the grammar), `Settings.h` (the setting keys),
`src/arduino/Console.cpp` (what each `#P` command does and prints) and `MeshProtocol.h` (the mesh).
On the board, `#PHELP` prints the short list.

---

## 1. Where commands come from

| Source | Line settings | What it may send |
|---|---|---|
| USB console | 115200 baud | everything |
| Command serial port (the Marcduino bus, RX on GPIO16) | 9600 baud by default (`#PBAUD`). The board answers on it only after `#PCMDTX1` (TX on GPIO0, only if you wired it) | everything except turning on `#POVERRIDE`, and `#PSETHOME` |
| The console on the web page | lines starting with `:` or `#`, up to 200 characters | everything |
| WCB mesh, when it is on (section 8) | one message is one line, up to 207 characters | `:P` moves and a few `#P` questions |
| Trigger inputs A, B, C | grounded = active | a change to the value 1-7 plays stored sequence 1-7 (`:PS<n>`) |

How a line is read:

- A line ends with CR or LF. On the serial ports it must start with `:`, `#` or `&` as its very first
  character (no leading space). Any other line (another board's heartbeat, chatter on the bus) is
  dropped whole, with no answer.
- `:P…` lines are motion. `#P…` lines are configuration and questions.
- A `:` line that is not `:P` (`:SE00` for the dome panels, for example) belongs to another board and
  is ignored silently, as is a `#` line that is not `#P`. A `:P` or `#P` line that does not parse is
  answered with `INVALID`.
- **Board ID.** A number straight after `:P` or `#P` addresses one board: `:P2P100` lifts board 2,
  `#P2STATE` asks board 2. A line with an ID runs only on the board whose `#PID` matches (the
  default ID is 0). A line without an ID runs on every board.
- **Chains.** `:` joins steps on one line: `:PP100:PW2:PA90:PW2:PH`. Up to 32 steps. After the first
  step the short forms `P100`, `A90`, `W2`, `L5` are accepted too. An ID goes on the first step only.
- **Duplicates.** The same line arriving on two different sources within 750 ms (`#PDEDUP`) runs
  once. The same source repeating a line always runs it again.

Replies are `KEY=VALUE` lines on the USB console and the page's log (and on the command serial
port with `#PCMDTX1`). The ones you will see most:

| Reply | Meaning |
|---|---|
| `QUEUED=<job> id=<n>` | the move was accepted |
| `<JOB> REFUSED <reason>` | the safety rules refused it, e.g. `LIFTER_SEEK REFUSED LOCKED`, `ROTARY_NOT_HOME`, `BELOW_SAFE_HEIGHT`, `LIFTER_UNKNOWN`, `ROTARY_DISABLED`, `ESTOP` |
| `SEEK CLAMPED <a> -> <b> reason=ROTARY_NOT_HOME` | a descent was stopped at the safe-spin height because the head may be turned |
| `INVALID`, `INVALID=<step>` | a malformed `#P` command, or a chain step that does not parse (nothing ran) |
| `OUT_OF_RANGE=#P<KEY>…` | a setting value outside its range (nothing changed) |
| `BUSY…` | the queue or the flash writer is full; send it again |

---

## 2. E-STOP

**What latches it.** On the USB console and the command serial port every byte is checked as it
arrives, before the line is read, so these work even mid-chain or while the board is busy:

| Pattern | Latches when |
|---|---|
| `:PX` | it ends the line (on its own, or as the last step: `:PP100:PX`) |
| `?STOP` | it ends the line |
| `&ESTOP` | it ends the line |
| `&SABE,ESTOP` | it ends the line or a comma follows (`&SABE,ESTOP,<reason>`) |

The mesh latches on the same patterns and also on `&ESTOP,<reason>`. The page latches from its
E-STOP button. A `:PX` typed in the page's console, or reached inside a stored sequence, latches too.
`:PX` needs no board ID; with one (`:P2X`) only that board stops.

**What it does.** Both motors brake, the drivers stay on until the mechanism is still and then switch
off, random mode ends and any chain is dropped. Every move is refused (`REFUSED ESTOP`) until the
latch is released. A second E-STOP while latched changes nothing.

**What releases it.** Only a deliberate action:

- the page's **Reset** button,
- `#PRESUME` from any source, including the mesh (`ESTOP=released`, or `ESTOP=not_latched`),
- `&ESTOP,RESET` on a serial port or from the mesh.

Nothing moves after a release until you send a move.

`#PRESUME` is deliberately a `#P` command and not a `:P` one: a chain or a stored sequence can only
hold `:P` steps, so no sequence can ever release an E-STOP.

**Stopping without latching:** the page's **Stop** button and the mesh verb `&DISABLE` end random
mode, drop the chain and cancel the running and queued moves. Nothing is latched.

---

## 3. The safety rules every move goes through

These apply to every source, every chain step and every random-mode action. Nothing bypasses them
except the rescue override, as the last point says.

- **LOCKED until commissioned.** A fresh board refuses every move (`REFUSED LOCKED`) until
  commissioning finishes, or until `#PUNLOCK` (a bench unlock).
- **The head never descends below the safe-spin height unless the rotary is home.** A descent with
  the head possibly turned stops at the safe-spin height. Until a safe-spin height is set (and the
  travel is known), only the top counts as clear.
- **The rotary never turns unless the mast is above the safe-spin height** (or on the top switch).
  Every rotation (`:PA`, `:PD`, `:PR`, `#PROTARYTEST`) first raises the mast to the top when it is
  not above that height.
- **With the lifter position unknown, only upward moves are allowed.** At power-up off a switch the
  position is unknown; `:PP100` or `:PH` references it.
- A new move interrupts a running seek, rotation, spin, jog or nudge. A home, a rotary test and a
  calibration always finish first.
- The 60-second rescue override (`#POVERRIDE`) relaxes the descent rule, and lets the rescue jog turn
  the head low, **only for commands typed at the USB console or on the page**. Moves from the command
  serial port, the mesh, trigger inputs or random mode never get it. Nothing relaxes the
  unknown-position rule.

---

## 4. Motion commands (`:P…`)

Speeds and powers are percent. `R` in place of a number picks a random value where shown.

| Command | Arguments | What it does |
|---|---|---|
| `:PP<pct>[,<speed>]` | height 0-100 % of travel; speed 0-100, or `R` (30-100). Omitted or 0 = full power for the motor profile; below `#PMINUP`/`#PMINDOWN` it is raised to that floor | Lift to that height. `:PP0` and `:PP100` run all the way to the switch |
| `:PPR[,<speed>]` | as above | Lift to a random height 0-100 % |
| `:PH[<speed>]` | speed 0-100, no comma (`:PH50`) | Safe lower: rises to the top first if the position is unknown or not trusted (or the head may be turned low), measures ticks/rev if never measured, homes the head unless it is already home, switches the light kit off, then lowers to the bottom switch. Use it to put the periscope away |
| `:PA<deg>[,<speed>][,<max>]` | any angle (taken as 0-359); speed 0-100 or `R` (40-100), default `#PROTARYMIN`; max 0-100 or `R`, default = speed | Turn to that angle by the shortest way. On a long move it runs at `max` and slows to `speed` over the last half turn. Homes first if home is not known; measures ticks/rev first if unknown. `:PA0` homes (always with the visible settle) |
| `:PAR[,<speed>][,<max>]` | as above | Turn to a random angle 0-359 |
| `:PD<deg>[,<speed>][,<max>]` | -36000 to 36000 degrees (100 turns); speed and max as `:PA` | Turn by that many degrees from where the head is, several turns if asked |
| `:PDR[,<speed>][,<max>]` | as above | Turn by a random -180 to 180 degrees |
| `:PR<speed>` | -100 to 100; the sign is the direction; raised to `#PROTARYMIN` | Spin until the next move. `:PR0` homes |
| `:PRR[<max>]` | max 1-100, default 80 | Spin at a random speed and direction up to max |
| `:PW<sec>` | 1-60 seconds; more is cut to 60 (`WAIT clamped`); no number = 6 s | Wait (inside a chain) |
| `:PWR<sec>` | 1-60 | Wait a random 1 to `<sec>` seconds |
| `:PL<mode>` | 0-7 | Light kit: 0 full cycle, 1 off, 2 Obi-Wan, 3 Yoda, 4 Sith, 5 searchlight, 6 Dagobah, 7 sparkle |
| `:PLR` | | A random light mode, never off |
| `:PS<n>` | 0-100 | Play stored sequence n (section 5.4) |
| `:PM` | | Random mode at the `#PAGGRESSION` level |
| `:PMG` / `:PMM` / `:PMA` | | Random mode gentle / medium / aggressive (the setting is not changed) |
| `:PM,<lift>,<rot>,<min>,<max>` | lift power (lifter floor to 100), rotary power (`#PROTARYMIN` to 100), pause min and max in seconds (1-600); any may be left empty (`:PM,80,,2,6`) | v3's parameterised random mode |
| `:PX` | | E-STOP (section 2) |

Rotation details:

- The height rules in section 3 apply: a rotation from below the safe-spin height raises the mast
  to the top first, and with `#PROTARYOFF1` every rotation is refused (`ROTARY_DISABLED`).
- `:PA` (other than `:PA0`), `:PD` and `:PH` on a board that has never measured its ticks per
  revolution run the two-turn measurement first (`#PROTARYTPR` or `#PROTARYTEST` sets it). `:PR`
  does not need it.

**Chains.** Each step waits for the one before it to finish; after a spin (`:PR`) the next step
starts as soon as the spin is running, so `:PP100:PR50:PW5:PA0` spins for five seconds and then
homes. A refused step ends the chain (`SEQ abort step 2/4: … REFUSED <reason>`). A new line with a
move or a wait replaces a running chain (`SEQ replaced`). A line with only `:PL` or `:PM` steps in
it runs at once and leaves a running chain alone.

**Random mode** (`:PM…`) keeps generating short chains: pop up and down, a big lift, a spin at the
top, a random angle, a look around, bob and weave (gentle uses only the big lift, the random angle
and the look around). Gentle lifts at 30-50 % power and pauses twice as long; medium 65-85 %;
aggressive 80 % to the profile's maximum, with short pauses. Every action that may descend homes
the head first. It ends on any move from outside random mode (`:PL` and `:PW` alone do not count),
on an E-STOP, the page's Stop, `&DISABLE`, a jog, a nudge, `#PSC`, `#PROTARYTEST`, commissioning,
a restart, or the first stall or driver fault. `#PRANDOMLIGHT` sets the light kit while it runs.
There is no "random off" command: send a move such as `:PH`. Note that `:PM0` starts random mode
(anything after `:PM` other than `G`, `M`, `A` or a comma is ignored). While commissioning is in
progress, random mode can only be started from the USB console or the page.

---

## 5. Configuration and diagnostics commands (`#P…`)

### 5.1 Questions

| Command | Prints |
|---|---|
| `#PSTATE` | four lines: `STATE=` (state, lifter count/travel and %, its confidence, rotary ticks and degrees, home confidence, ticks/rev); `LIMITS=` (switches, driver diagnostics, faults, E-STOP and its source, override seconds left, alignment); `JOB=` (running and last job with result); `SEQ=` (chain, random, light, inputs, flash) |
| `#PSTATUS` | version and build, uptime, serial settings and dropped lines, the I2C expander, WiFi, mesh |
| `#PCAL` | the lifter calibration: `CAL=` (travel, minimum powers, profile), `CALUP`/`CALDOWN` (speed and coast per 5 % power step), `CALREST`, `CALBRAKE` |
| `#PCONFIG` | every setting as a `#P<KEY>=<value>` line, then `#PSSID=`, `#PMESHMAC=`, which passwords are set, and the stored sequences. Paste it back to restore a board. A ticks/rev that was never confidently measured is written as `#PROTARYTPR=0`, so a restore measures it again |
| `#P<KEY>` | one setting with its help text, e.g. `#PSAFESPIN` |
| `#PHELP` | the command list and every setting key |
| `#PV3` | the v3 settings left in flash, and whether the one-time import has run |
| `#PCOMMISSION` | the commissioning step, whether it is active, and its last message |
| `#PL` | the stored sequences and their names |

### 5.2 Actions

| Command | What it does |
|---|---|
| `#PRESUME` | release the E-STOP |
| `#PCLEARFAULT` | ask for the latched faults to be cleared (`FAULT=clear_requested`) |
| `#PSC` | lifter calibration: raises and homes the head first if needed, then measures travel between the switch closings, the minimum powers and the braking table (`CAL stored: …`) |
| `#PROTARYTEST[<n>]` | rotary test over n turns (1-10, default 3): measures ticks per revolution and the switch arc, then homes. Raises to the top first if needed |
| `#PUNLOCK` / `#PLOCK` | bench unlock (saved; the board boots unlocked) / lock again |
| `#PR` | switch the rotary off or on (the same as `#PROTARYOFF`), saved, at once |
| `#PNCL` `#PNOL` | lifter limit switches normally closed / normally open (after `#PRESTART`) |
| `#PNCR` `#PNOR` | rotary home switch normally closed / normally open (after `#PRESTART`) |
| `#PV3IMPORT` | run the v3 import again; it only fills settings this board has no value of its own for |
| `#PRESTART` | restart once the motors are off and settings are saved |
| `#PWIFIRESET` | forget the home network, access point password back to `Astromech`, WiFi on, channel 1, restart |
| `#PFACTORY` | clear settings, calibration and stored sequences, then restart |

`#PSETHOME`, `#PV3IMPORT`, `#PRESTART`, `#PWIFIRESET` and `#PFACTORY` are refused while anything
is moving (`…=refused (something is moving; :PX or wait)`).

### 5.3 Rescue and commissioning

Lower a head that may be turned only in `#PNUDGE` steps, watching, and only above a height you
have measured it clears.

| Command | What it does |
|---|---|
| `#POVERRIDE` or `#POVERRIDE1` | 60-second safety override (section 3). USB console or page only; refused while the E-STOP is latched |
| `#POVERRIDE0` | end the override |
| `#PJOGL+` `#PJOGL-` `#PJOGR+` `#PJOGR-` | jog the lifter up/down or the rotary +/- for one 2-second lease (the page renews it while the button is held). An optional percent may follow (`#PJOGR+30`, 0-100; used as at least 12 %, and for the lifter at most the profile's cap); without it the jog runs at the slow power (the calibrated minimum for the lifter; two thirds of `#PROTARYMIN`, at least 12 %, for the rotary) |
| `#PJOG0` | release the jog now |
| `#PNUDGE<±ticks>` | move the lifter by that many ticks at the slow power, continuously (`#PNUDGE-20`, `#PNUDGE+20`). At most 100 ticks either way; larger values are cut to 100. Stops on a switch, a stall or the safety floor |
| `#PSETHOME` | the head is lined up by eye: make this position home. USB console or page only; refused while latched, with the rotary off, or while moving |
| `#PCOMMISSIONSTART` | start (or resume) the guided commissioning |
| `#PCOMMISSIONRUN` | run the current step's test |
| `#PCOMMISSIONNEXT` | go to the next step once this one has passed |
| `#PCOMMISSIONSTOP` | pause; the step is kept |
| `#PCOMMISSIONRESET` | back to the first step |
| `#PCOMMISSIONBYPASS` | unlock without commissioning (expert; `#PLOCK` undoes it) |
| `#PSAFESPINHERE` | commissioning step 6 only, with the head turned at least 45 degrees and the mast below the top: store this height plus a margin as the safe-spin height |

The steps are 1 profile, 2 wiring check, 3 lift test, 4 rotary, 5 calibration, 6 safe-spin height,
7 acceptance, then complete (`COMMISSIONED=2`). The page's Setup walks through them.

### 5.4 Stored sequences

| Command | What it does |
|---|---|
| `#PS<n>:<steps>` | store sequence n (0-100), e.g. `#PS3:P100:W2:A90:W2:H` |
| `#PSEQNAME<n>=<name>` | name it (up to 31 characters) |
| `#PD<n>` | delete sequence n |
| `#PZERO` | delete all stored sequences |
| `:PS<n>` | play it |

Steps may be any `:P` move, wait, light, random-mode or `:PS` step, and `:PX`; in the full form
(`:PP100:PL5`) or the short v3 form (`P100:L5`). The board stores the full form, at most 255
characters, with waits over 60 s cut to 60. A sequence may play another, up to 8 deep. A slot number
above 100 is treated as 100. On the serial ports a `#PS` line that ends in `:PX` also latches the
E-STOP as it arrives (section 2); store it from the page's console, or release with `#PRESUME`.

### 5.5 Settings

Set with `#P<KEY><value>` or `#P<KEY>=<value>` (`#PSAFESPIN878`, `#PTRAVEL=1137`); read with
`#P<KEY>`. The board answers `#P<KEY>=<value> (saved)` or `(unchanged)`; a value out of range gives
`OUT_OF_RANGE=…` and changes nothing. Settings are saved to flash once the motors are off. A change
takes effect at once (motion settings when the board is next idle) unless the last column says
**restart** (send `#PRESTART`). Defaults are the firmware's; on the first boot the one-time v3
import may take over some of your v3 numbers.

| Key | Values | Default | Meaning | |
|---|---|---|---|---|
| `MOTOR` | 0-2 | 1 | lifter motor: 0 Pololu 4757 (6.3:1), 1 Pololu 4751 (19:1), 2 IA-Parts. Changing it locks the board again, clears TRAVEL, SAFESPIN, MINUP, MINDOWN and APPROACH, sets the calibration aside until `#PSC`, and sends commissioning back to step 1 | |
| `INVERT` | 0/1 | 0 | the mechanism runs backwards | |
| `TRAVEL` | 0, 100-5000 | 0 | lifter travel in ticks; 0 = the calibration's figure, else the profile's guess | |
| `SAFESPIN` | 0-5000 | 0 | ticks above the bottom switch where the head clears the dome; 0 = only the top counts. Used only when the travel is known | |
| `ROTARYTPR` | 0, 1000-50000 | 0 | rotary ticks per revolution in tenths (11855 = 1185.5); setting it marks it measured; 0 = measure on the next rotary move | |
| `HOMEOFFSET` | -600 to 600 | 0 | ticks from the switch centre to the visual home | |
| `HOMEPARK` | 0-2 | 1 | 0 the arc centre; 1 against the sweep (v3's spot); 2 with the sweep | |
| `WIGGLE` | 0-20 | 2 | degrees of the first swing of the settle wiggle; 0 = none | |
| `MINUP` | 0-100 | 0 | % lowest lifter power going up; 0 = calibrated or profile | |
| `MINDOWN` | 0-100 | 0 | % lowest lifter power going down; 0 = calibrated or profile | |
| `APPROACH` | 0-100 | 0 | % power on the final approach to a limit; 0 = profile | |
| `EXPERT` | 0/1 | 0 | lift the 19:1 profile's power caps | |
| `DRIFT` | 0-20 | 5 | % drift before the hold re-seeks; 0 = off | |
| `ROTARYMIN` | 10-100 | 40 | % floor for rotary moves, and their default speed | |
| `ROTARYOFF` | 0/1 | 0 | 1 = no rotary fitted: no homing, descents do not wait for it | |
| `DRIVESIGN` | -1 or 1 | -1 | rotary drive polarity (-1 = v3 wiring) | |
| `AGGRESSION` | 0-2 | 1 | `:PM` level: 0 gentle, 1 medium, 2 aggressive | |
| `ID` | 0-255 | 0 | Marcduino board ID (section 1) | |
| `BAUD` | 1200-921600 | 9600 | command serial port speed; the port reopens at once | |
| `DEDUP` | 0-5000 | 750 | ms in which the same line from two sources runs once; 0 = off | |
| `DEBUG` | 0/1 | 0 | verbose motion tracing | |
| `CMDTX` | 0/1 | 0 | send replies on the command serial TX (GPIO0, only if wired); the port reopens at once | |
| `LIFTERNC` | 0/1 | 0 | lifter limit switches: 0 normally open, 1 normally closed | restart |
| `ROTARYNC` | 0/1 | 0 | rotary home switch: 0 normally open, 1 normally closed | restart |
| `WIFI` | 0/1 | 1 | the radio (access point and home network) | restart |
| `APCHANNEL` | 1-13 | 1 | the access point's WiFi channel | restart |
| `THEME` | 0/1 | 0 | web page: 0 light, 1 dark | |
| `RANDOMLIGHT` | 0-8 | 0 | light kit in random mode: 0 leave it alone, 1-7 that mode, 8 a new one each action; off when random mode ends | |
| `COMMISSIONED` | 0-2 | 0 | 0 locked, 1 bench unlock, 2 commissioned | |
| `MESHQTY` | 0-19 | 0 | how many WCBs | restart |
| `MESHID` | 0-19 | 0 | this board's mesh ID, 1-19, used by no other board | restart |
| `MESHCHAN` | 1-11 | 1 | the WCBs' WiFi channel | restart |
| `MESHCRC` | 0/1 | 1 | match the WCBs' checksum setting | restart |
| `MESHHB` | 5-300 | 10 | seconds between `&PERI,HB` heartbeats (read when the mesh starts) | restart |
| `MESH` | 0/1 | 0 | join the WCB mesh; the home network is then not joined | restart |

Text settings take `=` and the text: `#PSSID=My Network`. `#PSSID=` clears it.

| Key | Value | Default | Meaning | |
|---|---|---|---|---|
| `SSID` | up to 32 characters | empty | home network to join; empty = access point only | restart |
| `PASS` | up to 64 characters | empty | home network password (secret) | restart |
| `APPASS` | 8-32 characters (a longer one is cut to 32 without a warning) | `Astromech` | the board's own access point password (secret) | restart |
| `MESHMAC` | two hex octets, e.g. `3C:4E` | empty | the WCBs' MAC octets 2 and 3 | restart |
| `MESHPASS` | up to 39 characters | empty | the WCBs' mesh password (secret) | restart |

**Passwords.** `PASS`, `APPASS` and `MESHPASS` are never printed: `#P<KEY>` and `#PCONFIG` say only
whether they are set, so a pasted `#PCONFIG` does not restore them. Set them on the web page
(Setup), not by typing into a serial monitor: a monitor's log keeps every line you type. (The
board itself never prints a password back, and logs a `#P` line from the mesh without its text.)
The access point's default password, `Astromech`, is published: anyone nearby who knows it can join
and drive the periscope, so set your own.

---

## 6. Which source may do what

| Commands | USB console | Web page | Command serial | Mesh |
|---|---|---|---|---|
| `:P` moves, waits, lights, stored-sequence playback | yes | yes | yes | yes |
| `:PM` random mode | yes | yes | yes (not while commissioning) | yes (not while commissioning) |
| E-STOP: `:PX`, `?STOP`, `&ESTOP`, `&SABE,ESTOP` | yes | `:PX` and the E-STOP button | yes | yes, plus `&ESTOP,<reason>` |
| `#PSTATE` `#PSTATUS` `#PCAL` `#PHELP` `#PRESUME` | yes | yes | yes | yes |
| `#POVERRIDE` (on), `#PSETHOME` | yes | yes | refused | refused |
| `#POVERRIDE0`, `#PJOG…`, `#PNUDGE…` | yes | yes | yes | refused |
| Everything else: settings, `#PCONFIG`, `#PL`, `#PV3`, sequences, `#PSC`, `#PROTARYTEST`, `#PUNLOCK`/`#PLOCK`, commissioning, `#PRESTART`, `#PFACTORY`, `#PWIFIRESET` | yes | yes | yes | refused |

The mesh's `#P` list is exactly `#PSTATE`, `#PSTATUS`, `#PCAL`, `#PHELP` and `#PRESUME`; any other
`#P` line from the mesh is answered on the console with `MESH=refused a #P command from the mesh: …`.
Moves from the command serial port and the mesh go through the same safety rules as every other
source and never get the rescue override or the commissioning unlock.

---

## 7. Trigger inputs

The three expander inputs A, B and C read as a value 0-7 (grounded = active). When the value
changes to 1-7 and holds for three samples, the board plays that stored sequence (`TRIGGER=<n>`,
then `:PS<n>`). The value found at power-up only sets the starting point and plays nothing.

---

## 8. The WCB mesh

Off by default. To join, set `#PMESHMAC`, `#PMESHPASS` (on the page), `#PMESHQTY`, `#PMESHID`,
`#PMESHCHAN` and `#PMESHCRC` from a WCB's `?BACKUP` listing, then `#PMESH1` and `#PRESTART`. With
the mesh on, the board's access point moves to the mesh channel and the home network is not
joined: the page is then at `http://192.168.4.1` on the board's own WiFi. Until every value is set
the mesh does not start and `#PSTATUS` says what is missing.

### 8.1 What the board answers

| Message | What happens | Answer |
|---|---|---|
| `:P…` or `#P…` | handled like any other source (section 6); other boards' lines are ignored | log lines only |
| `&ESTOP`, `&ESTOP,<reason>` | E-STOP latched | `&PERI,ACK,ESTOP` |
| `?STOP` | E-STOP latched | none |
| `&ESTOP,RESET` | E-STOP released | `&PERI,ACK,ESTOP.RESET` (also when nothing was latched) |
| `&DISABLE` | stop everything, no latch (as the page's Stop) | `&PERI,ACK,DISABLE` |
| `&STATUS` | sends a heartbeat, a state line and a position line now | `&PERI,ACK,STATUS` |
| `&HELLO` | | `&PERI,HELLO,<fw>,<proto>` |
| anything else | nothing | none |

One mesh message is one line. A message longer than 207 characters, or with a line break or other
control character inside, is dropped whole (an E-STOP pattern in it still latches).

### 8.2 What the board sends

| Line | Sent | Fields |
|---|---|---|
| `&PERI,HB,<fw>,<uptimeS>,<state>,<height%>,<rotDeg>,<homed>` | to everyone every `#PMESHHB` seconds (10 by default) | firmware version, seconds since boot, state, height 0-100, angle 0-359, homed 1/0 |
| `&PERI,POS,<height%>,<rotDeg>` | to the controller (mesh slot 20, Sabé) when either changes, at most once a second | as above |
| `&PERI,STATE,<state>` | to everyone when the state changes | |
| `&PERI,FAULT,<name>` | to the controller, acknowledged; to everyone if the controller is not there | the new fault |
| `&PERI,ACK,<verb>` | to everyone, for `&ESTOP`, `&ESTOP,RESET`, `&DISABLE`, `&STATUS` | `ESTOP`, `ESTOP.RESET`, `DISABLE`, `STATUS` |
| `&PERI,HELLO,<fw>,<proto>` | to everyone, in answer to `&HELLO` | firmware version, protocol 1 |

- `<state>` is `LOCKED`, `IDLE`, `MOVING`, `ESTOP` or `FAULT`.
- An unknown height or angle is `-1`. `<homed>` is 1 when the head is within 10 degrees of home (or
  the rotary is switched off).
- Fault names: `LIFTER_DRIVER`, `ROTARY_DRIVER`, `DRIVER_REPEATED`, `LIFTER_STALL`, `ROTARY_STALL`,
  `ENCODER_DIRECTION`, `ENCODER_NOISE`, `LIMIT_CONTRADICTION`, `LIMIT_MISMATCH`,
  `HOME_SWITCH_STUCK`, `EXPANDER`, `TICK_OVERRUN`, `SAVE_DEFERRED`.
- The format `&PERI,NAK,<verb>,<reason>` is reserved; this firmware does not send it.

---

## 9. Notes for WCB users

- **Reading a WCB's settings.** On WCB firmware 6.2 and 7.0, `?EPASS`, `?MAC`, `?WCBQ` and
  `?WCBCH` **set** a value on that WCB. Do not type them to look something up. Read the values with
  `?BACKUP` instead.
- **Sending to the periscope from a WCB console.** With the periscope as mesh board 5, put `;W5`
  (one digit, no comma) in front of the command:

  ```
  ;W5#PSTATE
  ;W5:PP100
  ;W5:PH
  ;W5&ESTOP
  ;W5&ESTOP,RESET
  ```

- **E-STOP from a WCB on 7.0.1.** A `?STOP` typed on a WCB stops only that WCB's own timed sequence
  and is not passed on. Use `;W5&ESTOP`, or the page's E-STOP button.
- **The serial wire and the mesh together.** A command that reaches the periscope over both the
  mesh and the WCB serial wire (for example `;W2;S2` when it is on WCB2's port 2) within 750 ms runs once.
