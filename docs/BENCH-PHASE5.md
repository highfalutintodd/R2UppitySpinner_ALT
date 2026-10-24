# Bench procedure — Phase 5 (seek braking, then the mesh)

For Todd. Phase 5 has two halves, in one build. **Part A** is the code review's D5, which you
approved on 2026-10-04: a seek now brakes by the speed it actually reached, so short moves move
and every move comes to rest close to where it was sent, and a turned head sent down comes to
rest at or above the safe-spin height, never below it. **Part B** is the WCB mesh: off until you
turn it on, then the periscope is device 5 on your mesh (your answers, 2026-10-04: three WCBs on
6.2.0, octets 3C:4E, channel 1, checksum on, Sabé at 20, the `;W3;S4` wire stays). Do Part A
first, with the mesh off; Part B only once Part A has passed.

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
   It ends with `Sketch uses 1225487 bytes (62%)` (a few bytes either way is fine; the mesh code
   makes it about 41 KB bigger than the last build, and it stays off until Part B).
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
   `7d93e1d8`), once only `SETTINGS=migrated from schema 4 … to 5` and `PERSIST=saved settings`
   (the settings gained the mesh fields, all off), `CALIBRATION=loaded travel=1137 … rest=37`,
   the `CONFIG` line with `travel=1137 (calibrated) safespin=878 tpr=1185.0`, `WIFI joined
   "WordNet"` as before, and `MESH=off (#PMESH1 and #PRESTART …)`. *Success:* the same numbers
   as before the install; nothing was re-measured.
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

### Results 2026-10-24, Part A (build 055c60e5, installed from the page)

Every check passed. The log is `uppity-bench-phase5a-log.txt` on Todd's Desktop.

- **Install and boot (check 1):** the page upload went through, with the usual `TICK` lines while
  flash was written. Boot: `phase 5  build 055c60e5`, `SETTINGS=migrated from schema 4 (224
  bytes) to 5` and `PERSIST=saved settings` once, `CALIBRATION=loaded travel=1137 minUp=250
  minDown=150 rest=37`, `CONFIG … travel=1137 (calibrated) safespin=878 tpr=1185.3 …
  commissioned=2` (the same as the old build printed minutes before), `MESH=off`. WordNet joined on
  the third try (reasons 2 and 4, then joined), at 192.168.86.91 (DHCP; it was .69 on 2026-10-04).
- **Up and home (check 2):** the top switch closed at a count of 1117 for 1137. Power-up starts the
  count at −37, the deepest rest, and after an ordinary Down the mast rests higher than that: the
  known safe-side case (the board thinks the mast is lower than it is), put right at the top switch.
- **Presets, head home (check 3):** every one within 4 ticks.

  | Preset | Target | Came to rest | Off by |
  |---|---|---|---|
  | 75 % | 852 | 848 | −4 |
  | 50 % | 568 | 571 | +3 |
  | 25 % | 284 | 286 | +2 |
  | 50 % | 568 | 566 | −2 |
  | 75 % | 852 | 851 | −1 |

- **Short typed steps, head home (check 4):** every one moved (on the old build the 34- and 23-tick
  steps down did not), every one within 8.

  | Step | Target | Came to rest | Off by |
  |---|---|---|---|
  | `:PP80` up | 909 | 910 | +1 |
  | `:PP77` 34 down | 875 | 867 | −8 |
  | `:PP80` up | 909 | 909 | 0 |
  | `:PP78` 23 down | 886 | 883 | −3 |
  | `:PP80` up | 909 | 908 | −1 |
  | `:PP79` 11 down | 898 | 892 | −6 |
  | `:PP80` up | 909 | 908 | −1 |

  Down runs 3 to 8 past, up lands within 1: the screw back-drives going down, so the brake takes
  longer that way. All within the goal; no change needed.
- **Random medium (check 5):** about 85 s, 31 seeks to a height, each at rest between −7 and +5 of
  its target; every turn at the top, every descent below 878 after a home or with the head already
  home; no fault, refusal or clamp. **Stop** cut a descent short (`SEEK ABORTED CANCEL`) and
  cleared the chain.
- **E-STOP during a descent (check 5):** from the page (**Down**, then **E-STOP**; typing `:PP0`
  in the monitor and switching to the browser is slower than a sub-second descent). Twice:
  `lifter -59 ticks … drivers off after 255 ms` and `-62 … 248 ms` (2026-10-04: −51 to −60).
  The second latch stayed on until a second **Reset**; the page showed the banner all along, so the
  page and the board agreed.
- **The clamp, safe-spin borrowed at 1100 (check 6):** the head turned 296 ticks (90°) at the top.
  `:PP85` → `SEEK CLAMPED 966 -> 1100 reason=ROTARY_NOT_HOME`, `CREEP START` (the mast rested
  about 40 above 1100), `CREEP CLAMPED`, `ALIGNMENT suspect …`, `CLAMP came to rest at 1105, +5
  from the safe-spin height 1100`. The second `:PP85`: `SEEK REFUSED ROTARY_NOT_HOME`, nothing
  moved. **Not exercised on the droid:** the *seek* path against the floor (a turned head more than
  60 above it), because the top is only about 40 above 1100. It rests on its host tests; an optional
  re-run with a lower borrowed height is below.
- **Restore (check 7):** **Home the head** parked on the switch at about 1105, `ALIGNMENT ok again`;
  `#PSAFESPIN=878 (saved)`; `#PCONFIG` shows schema 5, `#PSAFESPIN=878`, the mesh keys at their
  defaults (`#PMESH=0`, `#PMESHQTY=0`, `#PMESHID=0`) and `MESHPASS empty`.

Also in the log, not from this build:

- **The access point's name is not stable.** It printed `R2Uppity-FC3F` today, on the old build
  and the new, and `R2Uppity-FB3F` on every boot through 2026-10-04. `Net::begin` builds the name
  from `WiFi.macAddress()` before `WiFi.mode()`; on core 3.3.4 that call returns without writing
  when the station interface does not exist yet (`NetworkInterface::macAddress`, `_esp_netif ==
  NULL`), so the two hex pairs come from an uninitialised buffer. In since Phase 3 (`67e6587`). It
  matters for Part B, where the page is only on this network. Fixed the same day: the name now comes
  from the factory address (`esp_read_mac(mac, ESP_MAC_WIFI_STA)`, which works before WiFi starts
  and is not changed by the mesh library). Todd's board's factory address ends `13:90` (as
  WordNet's router sees it), so the network is **`R2Uppity-1390`** from that build on.
- The two `E (2xx) gpio: gpio_pullup_en` lines at boot: the known ones from Phase 1.

**Optional, the seek path of the floor margin** (not needed before Part B). The same staging with
a borrowed height of 1050 instead of 1100: the mast rests about 90 above it at the top, so the
board seeks instead of creeping. Even if the margin failed completely, a seek clamped to 1050 could
overshoot by at most the full-speed run-on (about 56 on this lifter), to about 994; and if the clamp itself
failed, the target is 966. Both are well above 878. `#PSAFESPIN1050`, `:PP100`, head **90°** at the
top, `:PP85`; expect `SEEK CLAMPED 966 -> 1050`, `SEEK START`, `CLAMP came to rest at 10NN, +N`
with N zero or positive; then **Home the head** and `#PSAFESPIN878`.

---

## Part B — the WCB mesh

**What it is.** With the mesh on, the periscope joins your WCBs' ESP-NOW network as its own
board, number 5. From the mesh it takes `:P` moves (through the same safety rules as every other
source, and never with the rescue override or the commissioning unlock), the `#PSTATE`,
`#PSTATUS` and `#PCAL` questions, `#PRESUME`, and the DroidNet verbs `&ESTOP`, `&ESTOP,RESET`, `&DISABLE`,
`&STATUS`, `&HELLO`; `?STOP`, `&ESTOP` and `:PX` latch the E-STOP the moment they arrive, like on
the serial ports. Settings, commissioning, unlocking and restarts are refused from the mesh
(`MESH=refused …`). It sends `&PERI,HB,<version>,<uptime>,<state>,<height %>,<angle>,<homed>`
every 10 s to everyone, `&PERI,POS,<height %>,<angle>` to Sabé when it moves (at most once a
second), `&PERI,STATE,…` when the state changes, and `&PERI,FAULT,…` to Sabé (to everyone if Sabé
does not answer). Anything else on the mesh gets no answer.

**What it costs you.** The mesh library takes over the WiFi station, so **while the mesh is on
the board does not join WordNet.** The page is then only on the board's own network: join your
phone or laptop to `R2Uppity-1390` (its password as before) and open `http://192.168.4.1`. The
WordNet details stay stored; turning the mesh off brings everything back after a restart.

**What was tested.** On the Mac: 234 host tests (18 for the mesh: the message types, the E-STOP
patterns, the `#P` lines the mesh may and may not run, the `&PERI` lines and their timing, the
settings, the duplicate filter, a mesh `:PP0` with a turned head and the override on (it stops at
the safe-spin height), and a mesh E-STOP during a descent braking like every other E-STOP). A
second, independent review of the mesh code found six things, all fixed and tested: a message too
long for one line could be cut short and its last move still run; a line break inside a message
could reach the serial bus as a second line; a fault report had no way to tell that Sabé was off
the air; the page's mesh status could go stale; channels 12-13 were accepted though the library
ignores them; and a `#P` line with a leading space read as INVALID. The page's Mesh section was
driven in the browser against the simulated board. **Nothing has run on a
real mesh.** The library is Greg's newest, 1.17.1 (commit 534a6c9 from 2026-10-04, not yet tagged;
`UppityFirmware/libs/WCB_Client/VENDORED.md`).

**One thing I could not check from here.** Your WCBs' quantity is 3 and the periscope is 5. The
WCBs reach a board above their quantity once they have learned it from its announcements, which
the periscope sends; whether firmware 6.2.0 already does that is what step 13 finds out, with a
harmless question. If it does not, nothing is unsafe: the periscope still hears broadcasts and
sends its heartbeats, and step 13 says what the choices are.

**Rule for Part B:** the head is home (or the mast is at the top) for everything that moves, as
in Part A. The only motion sent over the mesh is `:PP100` (up, always allowed) and `:PH` (lower
safely: it homes the head first). The E-STOP test runs during a spin at the top, where every angle
is clear and you have all the time you need to press Enter.

You need: the WCB config tool or a serial console on one WCB (the DroidNet console works too:
anything that lets you type a WCB command), and Sabé powered as usual.

### First: install the build with the fixed network name

Part A ran on build 055c60e5, which names the board's own network from leftover memory (see the
Part A results). This build names it from the chip's factory address, so it is **`R2Uppity-1390`**
from now on and stays that way. Nothing else changed; your settings stay as they are.

1. In the Terminal app, bring the fix into your checkout:
   ```
   git -C "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3" merge --ff-only claude/phase5-mesh
   ```
   Success looks like `Fast-forward` and three files (`CLAUDE.md`, `docs/BENCH-PHASE5.md`,
   `UppityFirmware/src/arduino/Net.cpp`).
2. Build, one line at a time:
   ```
   cd "/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Arduino/Claude Code/R2UppitySpinnerV3"
   ```
   ```
   arduino-cli compile --profile uppity --output-dir ~/Desktop/uppity-build UppityFirmware
   ```
   It ends with `Sketch uses 1225495 bytes (62%)`.
3. In a second Terminal window, the monitor for all of Part B (a new log file):
   ```
   arduino-cli monitor -p /dev/cu.wchusbserial110 -c baudrate=115200 | tee ~/Desktop/uppity-bench-phase5b-log.txt
   ```
4. On the laptop's browser, open the page at the address in the monitor's `WIFI joined` line
   (`http://192.168.86.91` on 2026-10-24). With the periscope idle: **Setup → Firmware → Choose
   File**, `UppityFirmware.ino.bin` in the `uppity-build` folder on your Desktop, **Upload**.
   *Success:* `Written. The board is restarting…`, and the page reloads.
5. *Success in the monitor:* `phase 5  build …` with a new id, `SETTINGS=loaded schema 5` (no
   migration this time), the same `CALIBRATION` and `CONFIG` numbers as in Part A, `MESH=off`, and
   **`WIFI AP "R2Uppity-1390"`**. If your phone or laptop remembers `R2Uppity-FB3F` or
   `R2Uppity-FC3F`, you can forget those networks; they will not come back.

### Turn it on

8. On the page (still at the WordNet address): **Setup → Mesh**. Fill in, pressing **Save** after
   each and waiting for `Saved` under it:
   - **Mesh MAC octets:** `3C:4E`
   - **Mesh password:** your WCBs' mesh password (what `?WCBP` shows on a WCB). Type it **here
     only**: never into the Terminal monitor, which writes every typed line into the log file.
     After saving, the box shows `(set — type to change)`; the password is never shown or printed.
   - **How many WCBs:** `3`
   - **This board's mesh number:** `5`
   - **Mesh channel:** `1`
   - **Checksum:** On
   - **Heartbeat every:** `10`
   - **Join the mesh:** On. The first **Save** asks again, with the home-network warning; tap it a
     second time.

   The monitor shows each line, for example `#PMESHID=5 (saved) (takes effect after #PRESTART)`,
   and `MESH note: after #PRESTART the board joins the WCB mesh and stays OFF your home network…`.
9. Tap **Restart to apply** (tap twice). The board restarts; the page loses it, as expected.
10. Join the laptop or phone to the WiFi network `R2Uppity-1390` and open `http://192.168.4.1`.
    In the monitor, the boot now shows:
    - `WIFI mesh mode: access point on the mesh channel 1, home network not joined (it stays stored).`
    - a few lines starting `[WCB_Client]` from Greg's library, including a WARNING that device 5 is
      above the quantity 3 (expected: it is what step 13 checks);
    - `MESH=joined as id 5 (3 WCBs, octets 3C:4E, channel 1, checksum on); &PERI,HB every 10s …`
    *Success:* those lines, and no `MESH=not started` or `MESH=FAILED`.
11. Within a minute: `MESH WCB1 online`, `MESH WCB2 online`, `MESH WCB3 online`, and with Sabé on,
    `MESH controller 20 online`. Then on the page, **Setup → Mesh** shows `on as 5 · WCBs 3/3 · Sabé
    seen`, and **Diagnostics** has a Mesh row with the same. Type into the monitor:
    ```
    #PSTATUS
    ```
    *Success:* a line `MESH=on id=5 wcbs=3/3 online controller=online … hb=N …` with N growing by
    one every 10 s.
12. **The heartbeat in DroidNet.** In the DroidNet console (or Sabé's log), lines like
    `&PERI,HB,4.0.0-dev,123,IDLE,100,0,1` every 10 s (state, height %, angle, 1 = head home).
    *Success:* they appear. If DroidNet shows nothing but steps 11 and 13 work, the WCB the Pi is
    on may not be forwarding mesh broadcasts to its USB port; note it and go on.

### Reaching it over the mesh

13. **A harmless question first.** On a WCB's console, type:
    ```
    ;W5#PSTATE
    ```
    (`;W5` sends the rest to board 5.) *Success:* the periscope's monitor shows `MESH #P line from
    N` followed by the usual `STATE=…`, `LIMITS=…`, `JOB=…`, `SEQ=…` lines. **If nothing arrives
    within a few seconds,** try once more after a minute (the WCBs learn board 5 from its
    announcements, which repeat about once a minute). If it still does not arrive, stop Part B here
    and tell me: the WCBs on 6.2.0 then do not learn a board above their quantity, and the choices
    are a WCB firmware update (6.2.1 is Greg's latest release), raising the WCBs' quantity, or
    broadcasts only. I would not change the WCBs without talking it through.
14. **A refusal, harmless by design.** On the WCB console:
    ```
    ;W5#PSAFESPIN0
    ```
    *Success:* `MESH=refused a #P command from the mesh: settings, …` and nothing changes (type
    `#PSAFESPIN` into the monitor: it still says 878).
15. **Up, over the mesh.** Make sure the head is home (page: **Operate → Home**). Then on the WCB
    console:
    ```
    ;W5:PP100
    ```
    *Success:* `MESH :PP100 from N`, and the mast rises to the top. (If it was already at the top:
    `already there`.)
16. **E-STOP over the mesh, during a spin at the top.** With the mast at the top, type into the
    periscope's monitor:
    ```
    :PR40
    ```
    The head spins slowly. On the WCB console type `?STOP` and press Enter while it spins.
    *Success:* the spin stops at once, `MESH ?STOP from N: E-STOP latched` in the monitor, the
    page shows the red E-STOP banner, and nothing moves until it is released. **If the head keeps
    spinning,** press **E-STOP** on the page (that always works), note it, and try the DroidNet verb
    instead once it is released and spinning again: `;W5&ESTOP` (then the WCB does not pass `?STOP`
    on to a client board). Then release it from the mesh, on the WCB console:
    ```
    ;W5&ESTOP,RESET
    ```
    *Success:* `ESTOP=released (mesh &ESTOP,RESET from N)`, the banner goes, and nothing starts
    moving by itself. Then **Operate → Home** to bring the head home.
17. **&DISABLE, &STATUS and &HELLO.** Start a slow spin again (`:PR40` in the monitor), then on the
    WCB console:
    ```
    ;W5&DISABLE
    ```
    *Success:* `STOP=mesh &DISABLE`, the spin ends (not an E-STOP: no banner). **Operate → Home**.
    Then:
    ```
    ;W5&STATUS
    ```
    ```
    ;W5&HELLO
    ```
    *Success:* in DroidNet (or Sabé's log) a heartbeat, `&PERI,STATE,IDLE` and `&PERI,ACK,STATUS`
    right away, then `&PERI,HELLO,4.0.0-dev,1`.
18. **Lower safely, over the mesh.** Head home or not, on the WCB console:
    ```
    ;W5:PH
    ```
    *Success:* `MESH :PH from N`; the head homes first if it needs to, then the mast lowers to the
    bottom. In DroidNet the `&PERI,HB` line now ends `,0,0,1` (height 0, angle 0, home).
19. **Optional: the same command both ways.** If Sabé (or DroidNet) can send one button both
    through `;W3;S4` and to board 5, press one that sends `:PP100` both ways at once. *Success:*
    the mast moves once, and the monitor says `(a duplicate of the same line on the serial bus:
    ran once)` on one of them.

### Turn it off again (or leave it on)

20. If you want the board back on WordNet: page (at `http://192.168.4.1`) → **Setup → Mesh → Join
    the mesh: Off → Save**, then **Restart to apply**. After the restart it joins WordNet as before
    (`WIFI joined "WordNet" …`, and `MESH=off`). The mesh settings stay stored for next time.
    If you prefer the mesh on, nothing else is needed; the page stays at `http://192.168.4.1`.

### Send back (Part B)

The monitor log (a new file name is fine: change `phase5a` to `phase5b` in the monitor line), what
step 13 did, and a few `&PERI` lines as DroidNet showed them.

### Not in this build, on purpose

- **Sabé's side** (show `&PERI` on its dashboard, send `&ESTOP,RESET` on its own reset, route `:P`
  to board 5) is Sabé's repository and needs your go-ahead separately.
- **The DroidNet Command Library entry for 4.0** is ready in `tools/droidnet/` (validated with the
  library's own checker). Sending it to Travis is a pull request on his repository: a public step
  I will not take without your OK.
