# Bench procedure — Phase 5 (seek braking, then the mesh)

For Todd. Phase 5 has two halves. **Part A** is the code review's D5, which you approved on
2026-10-04: a seek now brakes by the speed it actually reached, so short moves move and every
move comes to rest close to where it was sent, and a turned head sent down comes to rest at or
above the safe-spin height, never below it. **The mesh parts (B onward)** follow once Part A has
passed; they are written after your answers about the WCB fleet.

## Part A — seek braking (D5)

**What changed.** Until now a seek started braking as soon as it came within the stopping
distance of its *full* speed: 56 ticks going down and about 30 going up on your lifter. A move
shorter than that never moved at all (a random move from 942 to 909 on 2026-10-04 ended where it
started), and longer short moves stopped up to that much early (284 asked, 301 reached). Now the
board watches how fast the mast is really going over the last 16 ms and looks up, in your own
`#PSC` calibration table, how long the brake takes at that speed (27 ms at the slow end going
down, 38 ms at the fast end, as your table measured). It starts braking that far ahead.

Whenever the head may be turned and the mast is above the safe-spin height, a descent also brakes
against that height with the same safety margin the jogs have used since 2026-10-04 (a quarter
more, plus a tick), so the mast comes to rest at or above it. That holds whether the target was
below the safe-spin height (the board clamps it there) or just above it (a target of 880 with 878
set). Close above that height a full-speed start cannot brake in time, so there it goes the slow
way:

| Where the turned head starts | What a descent does |
|---|---|
| more than 60 ticks above the safe-spin height | a seek, braking early by its speed, and against the height with the margin |
| 21 to 60 ticks above | the slow creep Rescue's jog uses (2026-10-04: came to rest +4 above 1100) |
| 20 ticks above or less | nothing moves; the log says `CLAMP came to rest at …` where it already is |

After the mast has stopped at that height, a second descent is refused until the head is home (as
after a clamped jog).

**What you will notice.** Short moves (a few percent) now move. Every seek to a height (not to a
switch) prints one more line when the mast has stopped: `SEEK came to rest at 909, +2 from its
target 909`. The boot line says `phase 5`.

**What was tested.** On the Mac: 216 host tests pass (14 new), each new one failing on the old
build except a sanity check of the simulator itself; the firmware compiles clean with every
warning on (1,183,503 bytes, 60 %). A separate review of the first version of this change found
that a turned head sent to a target *just above* the safe-spin height could overshoot below it on
the simulator (the old build never moved such short steps, so it was safe only by accident); that
is fixed and is now one of the tests. The simulator ran two lifters: a fast 6.3:1 and a copy of
yours (about 1.5 ticks/ms flat out, braking in about 40 ms down and 26 ms up, as your E-STOPs and
`#PCAL` showed), each with the table its own calibration stores. Every step from 10 to 400 ticks,
both ways, now moves; on the 6.3:1 every one comes to rest within 5 ticks, on the copy of yours
within 9 (2.5 on average; before, steps under about 60 ticks did not move and longer ones stopped
up to 29 short). A turned head sent down from anywhere 2 to 120 ticks above the safe-spin height
came to rest at or above it on all five simulated set-ups (before: up to 49 below), and so did one
sent to a target 0 to 12 ticks above that height from 15 to 120 ticks above it. **Nothing has
run on the droid yet.** Your lifter is not the simulator; the numbers you record below are the
real test.

**Rule for every step below:** the head is home for every descent. The one test with a turned
head borrows `#PSAFESPIN1100` and sends it to `:PP85` (966 ticks, still 88 above your measured
878), so even if the clamp failed completely the mast would stop at 966, clear of the frame.
Nothing turns the head below the top except step 6's `Home the head` at about 1100. Nothing is
held: every move is a fixed target.

### Install

1. In the Terminal app, bring the work into your checkout (one line):
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/phase5-mesh
   ```
   Success looks like `Fast-forward` and a list of files.
2. Build the file the page will send, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1183503 bytes (60%)` (a few bytes either way is fine).
3. In a second Terminal window, the monitor (leave it running for the whole of Part A):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase5a-log.txt
   ```
4. On the laptop's browser, open the page (`http://192.168.86.69`, or the address in the monitor's
   `WIFI joined` line). With the periscope idle: **Setup → Firmware → Choose File**, pick
   `UppityFirmware.ino.bin` in the `uppity-build` folder on your Desktop, **Upload**. The page says
   `Written. The board is restarting…` and reloads. If the page upload fails, note what it said and
   flash over USB instead (close the monitor first; same folder as step 2):
   ```
   arduino-cli upload --profile uppity -p /dev/cu.wchusbserial110 UppityFirmware
   ```

### Checks

1. **Boot lines.** `Uppity Spinner ALT v4.0.0-dev  phase 5  build …` (a new build id, not
   `7d93e1d8`), `SETTINGS=loaded schema 4`, `CALIBRATION=loaded travel=1137 … rest=37`, and the
   `CONFIG` line with `travel=1137 (calibrated) safespin=878 tpr=1185.0`. *Success:* the same
   numbers as before the install; nothing was re-measured.
2. **Head home, at the top.** On the page: **Operate → Up**, wait for it to stop, then **Home**.
   *Success:* the head ends home and the page shows no alignment banner.
3. **The presets, head home.** Tap these one at a time, each after the last has stopped:
   **75%**, **50%**, **25%**, **50%**, **75%**. After each, about half a second after the mast
   stops, the monitor prints `SEEK came to rest at NNN, +N from its target TTT` (the targets are
   852, 568, 284, 568, 852). *Success:* each one within about 10 ticks of its target. Write down
   the five `+N` numbers (or just send the log).
4. **Short typed steps, head home.** Type these into the monitor one at a time, each after the
   last has stopped (the head stays home throughout; 77 % is 875, just below 878, which is fine
   with the head home):
   ```
   :PP80
   ```
   ```
   :PP77
   ```
   ```
   :PP80
   ```
   ```
   :PP78
   ```
   ```
   :PP80
   ```
   ```
   :PP79
   ```
   ```
   :PP80
   ```
   These are steps of 34, 23 and 11 ticks, each way. *Success:* every one moves (before this build
   the 34- and 23-tick steps down did not move at all) and its `SEEK came to rest` line is within
   about 10 ticks of the target. The 11-tick steps may overshoot by a few ticks: that is the
   shortest move `:PP` can ask for (1 % of your travel), and it needs a full-power start to get the
   19:1 moving.
5. **A minute of random mode, then E-STOP mid-descent.** **Operate → Random medium**, watch for
   about a minute, then **Stop**. *Success:* it behaves as before (homes before it lowers below the
   safe-spin height, never turns low), with `SEEK came to rest` lines along the way. Then, head home:
   ```
   :PP100
   ```
   ```
   :PP0
   ```
   and press **E-STOP** on the page while the mast comes down. *Success:* `ESTOP braked to a stop:
   lifter -NN ticks` with NN about 50–60, as on 2026-10-04 (this part is unchanged; it is a sanity
   check). Then **Reset** on the page.
6. **The clamp, with a borrowed height of 1100.** Type, one at a time:
   ```
   #PSAFESPIN1100
   ```
   Answer: `#PSAFESPIN=1100 (saved)`.
   ```
   :PP100
   ```
   When the mast is at the top, tap **90°** on the page (the head turns at the top, as many times
   before). Then:
   ```
   :PP85
   ```
   *Success:* `SEEK CLAMPED 966 -> 1100 reason=ROTARY_NOT_HOME`, then `SEEK START` (or
   `CREEP START`: the mast rests about 60 ticks above 1100 at the top, right at the line between
   the two; either is fine, and the log tells me which ran), then about a second later `CLAMP came
   to rest at 11NN, +N from the safe-spin height 1100` with **N zero or positive** (I expect under
   10), and the page's banner about the head being turned. Then send the same line again:
   ```
   :PP85
   ```
   *Success:* nothing moves; the log says `REFUSED ROTARY_NOT_HOME`.
7. **Home the head, and put 878 back.** On the page tap **Home the head** (the head turns home
   where it is, about 1100, or rises first if it needs to). *Success:* the banner goes. Then:
   ```
   #PSAFESPIN878
   ```
   Answer: `#PSAFESPIN=878 (saved)`. Then `#PCONFIG` and check the `#PSAFESPIN=878` line in its
   answer. **Do not skip this step:** with 1100 left in place random mode would simply stay higher,
   but 878 is your measured value.

### Send back

The log file `uppity-bench-phase5a-log.txt` from your Desktop, and anything that looked or
sounded different (a move that should have moved and did not, a stop that looked abrupt).

### If something is off

- A `SEEK came to rest` more than about 15 ticks off, or a step that does not move: send the log;
  nothing about it is unsafe with the head home.
- In step 6, `+N` negative (it came to rest below 1100): stop there, tap **Home the head**, type
  `#PSAFESPIN878`, and send the log. At 966 at the very worst the head is still 88 ticks above the
  height you measured with the dome on.
