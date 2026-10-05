# Code review of the v4 firmware, October 2026

Reviewed: everything under `UppityFirmware/` (`src/core`, `src/sim`, `src/arduino`,
`UppityFirmware.ino`, `test/native`) and `webui/`, at commit `019228e` (the build on your droid,
`bad7e34f`, plus the bench notes). The v3 files in the repo root were read for reference and not
touched. Branch: `claude/code-review-2026-10`.

## In short

The design holds up. One policy, one supervisor that drives the motors, a latch for E-STOP, the
rest of the board talking to it through a queue: every path that can move a motor still goes
through that one place, and the review found no way around it. What it did find are places where
the supervisor believed something about the mechanism that was not true at that moment. That is
the same kind of hole as the two frame strikes and the 2026-10-04 home-switch window.

The three most serious, all fixed:

1. **A head coasting past home could be taken as home.** If a spin was interrupted (a new command,
   random mode's next step, `:PH`, an E-STOP) in the 30 ms after the head passed the home switch,
   the board re-zeroed its count on the still-moving head. The next `:PH` then skipped its home and
   lowered the head 11 to 20 degrees off. On the simulator this happened in about half of the
   interruptions timed into that window; on the droid it is a narrow window (a few percent of
   interrupted spins near home), and your logs show no sign of it yet.
2. **The command UART went deaf after `#PBAUD` or `#PCMDTX`, its E-STOP included.** Changing
   (or just re-saving) the command serial speed or echo, including by pasting `#PCONFIG` back
   through the page's restore, left the Marcduino bus unheard until a restart. A `:PX` from Sabé
   would have done nothing. USB and the web E-STOP were unaffected. You have not used the command
   UART yet, which is why it never showed.
3. **After a factory reset and a settings restore, the safe-spin height was measured against a
   guessed travel.** With no `#PTRAVEL` and no calibration, the top switch sets the count to the
   19:1 profile's guess of 1350 instead of your 1137, so every height reads about 210 ticks high
   and the head could turn about 210 ticks below 878. Your board as it stands (calibrated,
   `#PTRAVEL0`) was not exposed.

Twenty-four more defects are fixed below, most of them small. Each one that the Mac can run has a
host test that failed on the old code and passes now. **None of your measured settings were
changed**, and nothing changes in ordinary running. What you might notice:

- A restart (`#PRESTART`, the page's Restart) right after a move now waits up to about 2 s for the
  drivers to switch off and the save to land. The console already said it would.
- `#PV3IMPORT` now keeps your own numbers (1185.0 ticks/rev, 878, your powers) instead of
  replacing them with v3's.
- `#PAPPASS` with fewer than 8 characters is refused, with nothing changed.
- Lines meant for other boards on the Marcduino bus (`:SE00`, `:OP01`…) are silent now, as in v3.

**Four more changes you approved on 2026-10-04 (D1-D4) are in a second commit.** You would only
see them after a fault, in Rescue, or when pressing Stop with something queued:

- after a lifter stall, the head goes up to the top switch before it turns;
- the page's **Stop** (and commissioning's **Pause**) also cancels moves queued behind the running
  one;
- random mode turns itself off after a stall (`RANDOM=off (stalled)`);
- in Rescue, a held jog stops when the override that allowed it ends.

**What was tested:** host tests 202 pass (163 before, 39 new), and undefined behaviour now fails
the test run instead of printing a warning. The ESP32 build is clean with every warning on:
1,181,615 bytes (60 %), up 4,888 from 1,176,727. **Nothing here has run on the droid yet**; the
bench check at the end is short and keeps the head home or at the top.

### How the review was done, and how sure each finding is

Five reviewers, one per area: the safety core; everything that asks for motion (commissioning,
random, sequences, rescue, console and web); concurrency and the ESP32's resources; the parser,
settings and timing; the web API, the page and the tests as a whole. A second reviewer then went
through each area's findings trying to disprove them, writing simulator tests where possible. I
then re-traced every finding I fixed before changing anything.

- **Proven on the simulator**: a host test failed on the old code. These are the most certain.
- **By reading**: board-side code (`src/arduino`, the sketch, the page) cannot run on the Mac. Those
  were traced line by line, against the ESP32 core 3.3.4 source where it mattered.
- Two of the 64 reported findings were disproved (both about simulator behaviour it already
  models); many others were the same defect found from two or three directions.

---

## Fixed defects, most serious first

The number in brackets is the test in `UppityFirmware/test/native/test_review.cpp` (`reviewNN_…`).

### Critical

**1. The frame taken from a coasting head** [review01]
`src/core/MotionSupervisor.cpp:191-215` (the rest-contact rule in `updateKnowledge`).
*What could happen:* a spin interrupted just after the head passed the switch leaves the head
coasting off it. The switch input is debounced: it reads closed for 30 ms after the switch opens.
The rest-contact rule ("a closed switch with the motor at rest is a trusted zero") looked at the
job, not the head, so on the first tick after the job ended it re-zeroed the count wherever the
head happened to be (`HOME FRAME CORRECTED`). Every later "is the head home?" trusted that zero.
On the simulator with your switch's 50-tick arc, `:PH` then lowered the head 11-20 degrees off;
an E-STOP instead of a new command did the same.
*How sure:* proven on the simulator (all three variants: a referenced head, an unreferenced
head, an E-STOP).
*Fix:* a rest contact now needs the switch closed at that moment (the raw input as well as the
debounced one) and the count still for 50 ms. The policy's "on the switch" check reads the raw
switch too. A head that comes to rest on the switch is trusted as before (also tested).

**2. The command UART deaf after `#PBAUD` / `#PCMDTX`** (by reading)
`src/arduino/Console.cpp:129-137`, `src/arduino/SerialFront.cpp:12-20`.
*What could happen:* reopening the port calls `HardwareSerial::end()`, which in core 3.3.4 drops
the receive callback and its task; nothing registered it again. Commands from the bus, and the
bus E-STOP scanner (`:PX`, `&ESTOP`, `&SABE,ESTOP`), stopped until a restart. The console
printed "(command UART reopened)". The page's "Restore from saved text" replays `#PBAUD` and
`#PCMDTX`, so restoring settings was enough to trigger it.
*How sure:* certain (traced in the installed core source; confirmed by both reviewers).
*Fix:* the console re-registers the receive hooks after every reopen, and only reopens when the
value actually changed. Not testable on the Mac; Phase 5's bus bench covers it.

**3. Safe-spin against a guessed travel** [review07]
`src/core/MotionSupervisor.cpp:41-47` (`safeSpin()`).
*What could happen:* the safe-spin height is ticks above the bottom switch, but the top switch
sets the count to the travel in use. With only the profile's guess (after `#PFACTORY` and a
settings restore, or a first boot with v3's settings), each trip to the top made the count read
about 210 ticks high, so the head could turn about 210 ticks below the height you measured as
clear. That is the same kind of strike as 2026-09-19.
*How sure:* proven on the simulator.
*Fix:* until the travel is measured (`#PSC` or commissioning) or set (`#PTRAVEL`), only the top
counts as clear, as with no safe-spin height at all. Your board is calibrated, so nothing changes
for you.

### High

**4. The rescue override applied to every source** [review05]
`src/core/MotionSupervisor.cpp:104-121`, `src/arduino/Console.cpp:325-331`.
*What could happen:* during the 60 s override (Rescue, or commissioning step 6 with the head
turned 90 degrees below the top), a `:PP0` from the dome's bus, a trigger input, random mode or
(in Phase 5) the mesh lowered the turned head straight down. That is the 2026-10-03 crash, started
by something nobody at the page controls.
*How sure:* proven on the simulator.
*Fix:* the override, and the commissioning unlock (finding 11), now apply only to moves asked for
by someone watching: the USB console, the web page, and the commissioning flow's own steps.
`#PSC` and `#PROTARYTEST` now carry the source they really came from (they were always marked as
USB). `#PSETHOME` is accepted only from USB or the page, like `#POVERRIDE`.

**5. Switching the rotary off with the head turned** [review06]
`src/core/SafetyPolicy.h:86-91`, `src/core/MotionSupervisor.cpp:133-145`.
*What could happen:* `#PR` (three characters, a v3 command) or `#PROTARYOFF1` (Setup → "No rotary
fitted") made the head count as home at once and cleared "the head may be turned". The next
descent lowered a turned head into the frame. The likeliest moment is a rescue where the head will
not turn, which is exactly when the head is turned.
*How sure:* proven on the simulator.
*Fix:* switching the rotary off no longer clears "the head may be turned". Descents stop at the
safe-spin height until a restart, or until the rotary is back on and homed. The console says so
when it happens. A board that powers up with the rotary off behaves as before (tested).

**6. A stall that starts on the home switch kept the frame trusted** [review02]
`src/core/MotionSupervisor.cpp:554-558` and `:572-575`.
*What could happen:* the head normally parks on the switch. If the rotary count stops while the
head turns (the encoder disc slipping, a loose lead; you have an open item on the coupler and
disc), the job ends as a stall. From that parked state the board still trusted the frozen count,
so the next `:PH` lowered the head about 90 degrees off on the simulator.
*How sure:* proven on the simulator.
*Fix:* a stall, or a failed home, marks the frame STALE from the parked state as well, so the next
descent homes first.

**7. A home switch stuck closed** [review03]
`src/core/MotionSupervisor.cpp:221-249` (`watchHomeSwitch`), `:427`.
*What could happen:* a switch that reads closed everywhere (a stuck microswitch, a shorted lead)
makes every resting angle look like home. `:PA90` then `:PH` lowered the head 85 degrees off on
the simulator. The home routine itself would have accepted the stuck reading as an edge.
*How sure:* proven on the simulator.
*Fix:* during every rotary job the board watches how far the head turns while the switch reads
closed without a break. Past twice the contact arc plus 20 ticks (on your rotary about 125 ticks,
38 degrees, more than twice the real arc) it raises `HOME_SWITCH_STUCK`. The frame goes STALE,
homes are refused, and a turned head stays above the safe-spin height. It clears once the switch
reads open at rest. On the simulator, with your switch's 50-tick arc, hard spins both ways never
trip it; the bench check repeats that on the droid. **Limit:** a switch already stuck at
power-up cannot be told from a head parked on it until the head turns.

**8. Settings from a newer build were overwritten with defaults** [review12]
`src/core/Settings.h:104-127`, `src/arduino/Persist.cpp:48-56`.
*What could happen:* flashing an older v4 build over a newer one (going back a step after Phase 5,
say) read the newer settings as "unreadable" and saved defaults over them at the first idle
moment: INVERT 0, safe-spin from v3, LOCKED. Going forward again would not bring them back.
*How sure:* proven on the host (the settings migration). The write-back was traced by reading.
*Fix:* a newer blob is read for the part this build knows (the layout only grows at the end) and
is not written back. The calibration blob is handled the same way.

**9. An interrupted page upload stopped all motion until a setting was saved** (by reading; the
holder logic proven) [review13]
`src/arduino/Net.cpp:378-384`, `src/core/MotionSupervisor.h:117-127`.
*What could happen:* if the phone locked or the WiFi dropped during Setup → Firmware, the board
kept the old firmware but quietly stopped starting moves. The next settings save released the hold
and the queued, stale commands ran at once. A settings save in the middle of a good upload also
released the upload's hold.
*Fix:* an aborted upload releases its hold, and each holder (saving, uploading, restarting) now
has its own hold, so one cannot release another's.

### Medium

**10. The override and the jog lease came back after 24.9 days** [review04]
`src/core/CommandQueue.h:52-76`. A stored deadline was compared by signed difference and never
cleared, so 2^31 ms after it ended it read as live again. The override then bypassed "the head
must be home" for the next 24.9 days. Only reachable on a board left powered for weeks (a display
droid). Proven. Fix: the tick clears an expired lease within a millisecond; 0 means none.

**11. Commissioning unlocked the board for every source** [review05]
`src/core/MotionSupervisor.cpp:97-101`, `src/core/Sequencer.h:192-202`. While commissioning ran
on an uncommissioned board, bus `:P` lines and trigger inputs could move it before its direction,
rotary and travel were known. Not your board (commissioned). Proven. Fix: the unlock covers the
flow, the page and USB; random mode (which step 7 runs) may only be started by them meanwhile.

**12. The drift hold survived an E-STOP and `#PINVERT`** [review08]
`src/core/MotionSupervisor.cpp:20, 710`. An E-STOP pressed while the hold was correcting (motion
nobody asked for) came back about 5 s after the reset; flipping "runs backwards" after a preset made
the mast drive by itself the wrong way. Proven. Fix: both end the hold.

**13. Other boards' Marcduino lines answered `INVALID=`** [review11]
`src/core/Dispatcher.h:76-81`. `:SE00`, `:OP01` and similar printed `INVALID=…` on USB, in the
log and (with `#PCMDTX1`) back onto the bus. Hard rule 7 and v3 say silent. Proven. Fix: a `:`
line that is not `:P…` is silent; a broken `:P` line still says `INVALID`.

**14. Restarts did not wait** (by reading)
`src/arduino/Console.cpp:189-215`. `#PRESTART` and the page's Restart fired 300 ms later
regardless. A setting changed within 2 s of a move (the save waits for the drivers to switch off)
was lost although the console said "waiting for PERSIST=saved first". Random mode could start a
move inside those 300 ms, and `#PFACTORY` erased the flash with the drivers possibly still on (hard
rule 5). Fix: the restart stops random mode and the chain, holds new moves, and waits until nothing
runs, the drivers are off and the save has landed (5 s at most, with a log line if that runs out).
The factory erase runs at that moment.

**15. A short `#PAPPASS` became "Astromech"** [review12]
`src/core/Settings.h:318-327`, `src/arduino/Console.cpp:408-409`. A 1-7 character access-point
password (USB or `/api/cmd`; the page already blocks it) was replaced with the published default
and saved, which is also the network OTA password. Proven. Fix: refused, nothing changed.

**16. `#PV3IMPORT` replaced measured numbers** [review12]
`src/core/Settings.h:363-420`. A forced import took v3's 1191 ticks/rev over your 1185.0, 904 over
878, and 50 % minimum powers over a calibration. Proven. Fix: a forced import only fills settings
the board has no value of its own for, and lists what it kept. The first-boot import is unchanged.

**17. A slow `loop()` ended a move in a sticky "expander" fault** [review14]
`src/core/MotionSupervisor.h:68-71`, `UppityFirmware.ino:93-96`, `src/arduino/Console.cpp:133`.
The drivers are switched on from `loop()`, and the supervisor gave that 50 ms. A long print
(`#PCONFIG` is about 60 ms at 115200; with `#PCMDTX1` at 9600 much longer) could make a move fail
with a fault only `#PCLEARFAULT` cleared. Proven (simulated slow acknowledgement). Fix: the serial
ports get transmit buffers so prints do not hold up `loop()`, and the wait is 250 ms. A dead
expander still faults.

**18. Lost serial bytes could splice two lines into one valid command** [review11]
`src/core/LineAssembler.h:39-90`, `src/arduino/SerialFront.cpp:12-20`. If `loop()` fell behind a
paste, or the UART's own buffer overflowed during a flash write, the head of one line joined the
tail of the next: `#PSAFESPIN878` + `#PDRIFT=0` read as `#PSAFESPIN870`. Proven for the software
buffer; the UART overflow is by reading. Fix: a lost byte now spoils the line it belonged to, which
is then dropped whole.

### Low

| # | Where | What | How sure | Fix |
|---|---|---|---|---|
| 19 | `LineAssembler.h:71-82` | A line over 255 characters was cut short and run (a long chain ending `:PP100` became `:PP1`). | Proven [review11] | Dropped whole; the limit is 271 so the longest `#PS` line still fits. |
| 20 | `Sequencer.h:138-143` | A stored sequence that no longer parses (flash damage, a later grammar) crashed the sequencer. | Proven (crash) [review10] | Skipped with `SEQ n: unreadable, skipped`. |
| 21 | `RandomDirector.h:47-50` | Random mode is stopped on an E-STOP only through an event that a full event ring could drop. | Traced [review10] | The director also stops itself when it sees the latch. |
| 22 | `Sequencer.h:24-36, 120-130` | Huge typed numbers overflowed: `:PW4294968` waited 0.7 s, speeds ×10 overflowed. | Proven [review10] | Clamped before the arithmetic; undefined behaviour now fails the tests. |
| 23 | `MotionSupervisor.cpp:817-823`, `EstopLatch.h:17-25` | An E-STOP from the page landing mid-tick could be followed by up to 1 ms more drive. | Proven [review09] | The latch is read again right before each output. |
| 24 | `TextSink.h:11` | `#PCONFIG` cut stored sequences over about 190 characters (replay stored them shorter). | Proven [review15] | Larger print buffer. |
| 25 | `MotionSupervisor.cpp:10-21` | Settings handed to the supervisor could be read half-copied (two saves within a millisecond of a job ending). | By reading | Handed over under the queue's lock. |
| 26 | `Persist.cpp:108-139` | A failing flash write was retried and logged on every `loop()` pass. | By reading | Retried every 10 s, logged once. |
| 27 | `index.html:398-402, 562-566`, `Net.cpp:345-356` | An error answer from the board replaced the status for one poll; a log chunk full of control characters (line noise) could not be sent, and the page then re-read the whole log as new. | By reading; page checked on the dev server | The page ignores error answers; the board falls back to a smaller log chunk. |

---

## Your decisions

### Done, as you approved on 2026-10-04 (second commit)

**D1. A lifter count that stopped counting was still trusted** (high; proven on the simulator)
[reviewD1 ×3]. `src/core/SafetyPolicy.h:93-99, 116-118, 140-142`, `src/core/MotionSupervisor.cpp:218-220`.
After a lifter stall the count is marked STALE, but the policy treated STALE as known. If the lifter
encoder drops out during a descent (a loose lead), the mast falls on while the count stays high; the
next rotation (random mode, say) could then turn the head below 878, and each further `:PP` down with
the head turned drove blind for the stall time. *Now:* a STALE count is never "above the safe
height", so the next rotation first raises to the top switch (which makes the count right again), and
a descent with the head not home is refused until then. The first blind drop after the dropout cannot
be prevented by any rule; the ones after it are. *You would see:* only after a lifter stall.

**D2. The page's Stop, and commissioning's Pause, left queued moves to run** (high; proven)
[reviewD2]. `src/core/CommandQueue.h:52-57`, `src/core/MotionSupervisor.cpp:691`,
`src/arduino/Console.cpp:35, 57`. Press **Home the head**, then a preset, then Stop during the home:
the preset ran. Pause during commissioning step 5's home let the whole calibration run. *Now:* Stop
and Pause end the running job and everything queued; each queued move is logged `REFUSED CANCEL`.

**D3. Random mode drove into a jam every second or two** (high; proven) [reviewD3].
`src/core/Sequencer.h:113-120`, `src/arduino/Console.cpp:319`. 74 lifter stalls in 2 minutes on the
simulator, each pushing at show speed for 0.7 s. *Now:* the first stall or driver fault in a random
action ends random mode (`SEQ abort … STALLED`, then `RANDOM=off (stalled)`). A refusal does not:
at power-up a descent is refused until the lifter is referenced, and random mode recovers from that.

**D4. A held jog outlived the override** (medium; proven) [reviewD4 ×2].
`src/core/MotionSupervisor.cpp:362-368, 406-408, 799-805`. The policy is asked when a job starts.
A rotary jog held inside the dome under the override kept turning after the 60 s ran out (or after
**End now**); a Down jog with the head turned kept going below the safe-spin height. *Now:* a jog
that only the override allowed stops the moment the override ends (the log says `… ABORTED POLICY`). A jog
the rules allow anyway (above the safe height, or the head home) is not affected.

### Still open

**D5. Short seeks stop short, and a clamped seek has no margin** (known since Phase 1; needs a
bench). `src/core/LifterAxis.h:81`, `src/core/MotionSupervisor.cpp:333`. A seek brakes at the
calibrated run-on for its full speed even when it never reached that speed (56 ticks going down on
your lifter), so a short `:PP` step does not move and longer ones stop early. The same brake point,
used against the safe-spin floor for a turned head, has no safe-side margin (a creep has a quarter
extra); on your calibrated lifter today it stops short, which is the safe side, and your 878
includes a 34-tick margin. *Change:* brake by the speed actually reached, from the same
calibration table, with the creep's quarter margin when the target is the safe-spin floor. It
changes every seek, so it needs its own bench run with the head home. **Recommend: next session,
with a bench section of its own.**

**D6. A wrong `#PTRAVEL` moves every height** (medium). `src/core/MotionSupervisor.cpp:155-163`.
Finding 3 covers the profile guess. A `#PTRAVEL` larger than the real switch-to-switch distance
does the same thing on a smaller scale (the old rest-to-rest 1192 against 1137 would be 55 ticks),
and the board only cross-checks a calibrated travel against a 25 % error. *Option:* raise
`LIMIT_MISMATCH` when the top switch closes more than about 40 ticks from where the count expects
it, and keep the count. You use `#PTRAVEL0`, so this matters for other builders; the threshold has
to clear your power-up difference (the board starts at −37, an ordinary Down rests about −21).
**Recommend: Phase 5 or later, with numbers from your logs.**

**D7. Jog presses are not tied to their release** (low). `src/arduino/Net.cpp:293-304`. A start
request delayed by a lost packet can arrive after the page's releases and jog for the 2 s lease
with nobody holding; two phones share one lease. The 2 s bound holds. *Option:* a press number
carried by start, renew and release. **Recommend: later.**

**D8. `#PCONFIG` does not carry whether ticks/rev was measured** (low). `src/core/Settings.h:277`.
A figure that came from v3 (not measured) replays as measured. Yours is measured. *Option:* print
`#PROTARYTPR=0` with an INFO line when it was not measured. **Recommend: with Phase 5's settings
changes.**

Also noted, no change proposed: power-up on the top switch counts from the switch's closing point,
while the mast rests a little above it, so the height reads a little low until the next switch. That
is the safe direction (the mirror image of the `bottomRest` fix).

---

## Tests

- **39 new tests** in `test/native/test_review.cpp`, one per finding or variant (7 of them for
  D1-D4), each failing on the old code (the failing runs are kept in the session notes) and passing
  now. Five guard tests
  check that the fixes did not cost anything (a head resting on the switch is still trusted, the
  page and USB still get the override, a working switch never reads as stuck, a board without a
  rotary lowers freely, "set home here" on the switch is not a stuck switch).
- **A ground-truth watch in the test rig** (`rig.h`, `watchSafe`): every simulated millisecond it
  checks the mechanism itself, not the firmware's idea of it: no rotary drive below the safe-spin
  height, no descent past it with the head more than 10.6 degrees off. Checked by breaking the
  rotary rule on purpose: the watched test fails.
- **Tests that could not fail, repaired:** the "rotary only at the top" check read the switch after
  the move had ended; the random-mode "policy held" check read the final snapshot only (both now use
  the watch); "both limits closed at boot" booted with neither switch closed; the jog-lease wrap test
  never looked 2^31 ms out.
- **Undefined behaviour now fails `make test`** (`-fno-sanitize-recover`); before, a sanitizer report
  printed and the suite still passed. The current suite has none.
- **Still not covered** (for later): the console and web glue (`Console.cpp`, `Net.cpp`) has no
  host tests, which is why findings 2, 9, 14 and 26 are "by reading"; moving their decisions into
  the core would make them testable. The simulator is still kinder than your mechanism in a few
  places: no lifter backlash, no home-switch flicker when the mast thumps a limit, no expander loss
  in the middle of a move. The new tests use your switch's 50-tick arc; the older ones use the
  simulator's 16.

## Tidy-ups

- `SafetyPolicy.h`'s header said the policy is re-checked every tick; it is checked when each step
  starts. Corrected; the one exception is now D4 (a jog only the override allowed is re-checked every tick).
- Log and USB lines from the web task can interleave mid-line with `loop()`'s (cosmetic).
- No stack high-water marks are reported; the port-81 E-STOP task (4 KB) and the UART event task
  (2 KB, runs the panic hook) have never been measured. Estimated well inside; worth a line in
  `#PSTATUS`.
- An over-long `/api/config` value answers "busy" instead of "invalid"; an unknown URL redirects to
  192.168.4.1 even for a phone on the house network.
- The dev server's fixture now shows your numbers (1137, 878, 1185.0, commissioned 2).

## Checked and found sound

Every motion source reaches a motor only through the queue, the supervisor and the policy; the only
non-zero PWM write is in the supervisor's tick; only `I2cService` touches `Wire`; only `Persist`
writes the flash (apart from `#PFACTORY`, now moved under its rule). E-STOP: latched ticks brake
both axes, flush the queue every tick and end the override and the jog lease; nothing resumes after
a release. Every set and clear of "the head may be turned" and of the clamp hold was traced. Position
references (0 at the bottom switch's closing, the travel at the top's, the calibration's ends,
power-up with `rest=37`) agree. Settings reach the supervisor only between programs. Every `millis()`
comparison other than the two lease deadlines was wrap-safe. The status snapshot and the event ring
were stress-tested on two threads (no torn reads, nothing out of order). No password appears in any
reply, log line, `#PCONFIG` output or page. Every string the page puts on screen is escaped. The
port-81 E-STOP listener trips the latch before it answers anything. The 1 kHz tick does no heap or
`String` work. `WebAssets.h` matched `index.html` byte for byte (and is regenerated here).

---

## Bench check for this build

Nothing here can reach the frame even if a fix failed: rotations happen at the top only, and the
head stays home for every descent. About 20 minutes.

### Install

1. In the Terminal app, one line at a time:
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/code-review-2026-10
   ```
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1181615 bytes (60%)`.
2. In a second Terminal window, the monitor (leave it running for the whole check):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-review-log.txt
   ```
3. On the page: **Setup → Firmware → Choose File**, `UppityFirmware.ino.bin` in the `uppity-build`
   folder on your Desktop, **Upload**. The board restarts on the new build.

### Checks

1. **Boot lines.** The monitor shows `SETTINGS=loaded schema 4`, `CALIBRATION=loaded travel=1137 …
   rest=37`, and the `CONFIG` line with `travel=1137 (calibrated) safespin=878 tpr=1185.0`, as
   before. *Success:* the same numbers as before the install.
2. **Spins at the top, hardest case for the stuck-switch watch.** Type these into the monitor one at
   a time, waiting for each to finish:
   ```
   :PP100
   ```
   ```
   :PR100
   ```
   After about 5 seconds of spinning:
   ```
   :PR0
   ```
   (`:PR0` stops the spin and homes the head.) Then the other way:
   ```
   :PR-100
   ```
   ```
   :PR0
   ```
   *Success:* both spins stop and home as before, and the log has **no** `FAULT … home switch stuck
   closed` line and no `HOME FRAME CORRECTED` line.
3. **Ordinary running, as in the last install check.** On the page: the height presets and the
   rotary presets, then one minute of random mode at medium, then **Stop**, then `:PH` typed into the
   monitor. *Success:* homes and lowering behave as before; again no `home switch stuck` line.
4. **E-STOP during a descent, head home** (as in re-test 2 on 2026-10-04). With the head home, type:
   ```
   :PP100
   ```
   then
   ```
   :PP0
   ```
   and press **E-STOP** on the page while it comes down. *Success:* `ESTOP braked to a stop:
   lifter -NN ticks` with NN about 50-60, as on 2026-10-04, and no `LIMIT BOTTOM made` straight
   after. Then **Reset** on the page.
5. **A restart waits for its save.** On the page, **Operate → Up**. The moment it stops, change the
   theme (light/dark) and type into the monitor straight away:
   ```
   #PRESTART
   ```
   *Success:* `RESTART=waiting for PERSIST=saved first …`, then `PERSIST=saved settings` about 2 s
   after the move, then the board restarts, and the page comes back in the new theme. Change the
   theme back afterwards.
6. **Stop also cancels what is queued (D2), all at the top.** Type:
   ```
   :PP100
   ```
   When the mast is at the top, press **180°** on the Operate page and let the head get there. Then,
   quickly one after the other: **Home the head**, **90°**, **Stop**. *Success:* the head stops
   where it is and does not turn to 90°; the log shows a line with `REFUSED CANCEL`. (Before this
   build the 90° turn ran after the Stop. At the top every angle is clear, so this is harmless
   either way.) Then press **Home the head** once more.
7. **Optional: an interrupted upload.** **Setup → Firmware**, choose the same file, **Upload**, and
   reload the page when the progress passes about 30 %. Then **Operate → Up**. *Success:* the mast
   moves (before this build nothing moved until a setting was saved). The board keeps running the
   build you installed in step 3: an interrupted upload never replaces it.

### Send back

The log file `uppity-bench-review-log.txt` from your Desktop, and anything that looked different.

---

## Record

- Host tests: `make -C UppityFirmware/test/native test` → `OK: 202 tests passed` (UBSan, failing on
  any report).
- ESP32: `arduino-cli compile --profile uppity UppityFirmware` → `Sketch uses 1181615 bytes (60%)`,
  no warnings in the v4 sources with `--warnings all`. Web page rebuilt: 81,713 → 24,762 bytes
  gzipped.
- D1, D3 and D4 are not in the bench check: each needs a stall or the head inside the dome to show,
  and none can be staged where a failure would be harmless. The simulator covers them.
- Not run on the droid.
