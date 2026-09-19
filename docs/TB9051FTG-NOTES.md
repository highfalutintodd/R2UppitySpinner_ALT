# TB9051FTG facts the firmware depends on

From Toshiba's datasheet Ver.1.1 (2019-03-14), copy in `docs/datasheets/`. Read 2026-09-07 to
settle the brake-versus-coast question in the rewrite plan. Section numbers refer to the datasheet.

## Output truth table (Table 7.1-1, Table 7.2)

| PWM1 | PWM2 | EN | ENB | OUT1 | OUT2 | Meaning |
|---|---|---|---|---|---|---|
| H | L | H | L | H | L | Forward |
| L | H | H | L | L | H | Reverse |
| **L** | **L** | H | L | **L** | **L** | **Short brake** (both low-side FETs on) |
| H | H | H | L | L | L | Short brake (same output state) |
| X | X | L | X | Z | Z | Disabled: High-Z (coast) |
| X | X | X | H | Z | Z | Disabled: High-Z (coast) |

So v3's "stop" (PWM1=PWM2=0) has always been a brake, which is why pulsed drive decelerates the
lead screw so effectively: every off period shorts the winding. True coast (High-Z) needs EN low
or ENB high, and on the Uppity Spinner PCB those pins are on the I2C expander. In v4:
`Bridge::Brake` = both PWM low (the PWM-level stop, usable inside the 1 ms tick);
`Bridge::Coast` = enable-off request (I2C, applied by loop()), PWM low meanwhile.

## Direction reversal must pass through brake (7.1 Note 1, 7.4 note)

"When the motor is set to the reverse from the forward, or to the forward from the reverse, be
sure to perform after setting the brake between them. Otherwise the IC may be broken." The
internal dead time (4 µs typ) is not a substitute. v4 enforces it twice: the supervisor inserts a
brake tick between opposite-direction drives, and `Esp32Hal::writeBridge` writes both PWM low
and waits 20 µs before a reversed drive.

## DIAG (7.2, 7.7, 7.8)

Open-drain, pulled up to the MCU supply. It is **low** for: EN disable, ENB disable, VBAT
undervoltage, VCC undervoltage, VCC high voltage, over-temperature, over-current, the ~80 µs
initial diagnosis after power-on **and after every disable→enable transition**, and POR.

Consequences:
- DIAG low while the drivers are disabled is normal. The fault model only reads DIAG as a
  fault while the enable is acknowledged and at least a couple of milliseconds old.
- **Over-temperature (TSD, 175–200 °C) and over-current latch DIAG low** and hold the outputs
  High-Z. Neither auto-recovers. The latch clears on a **rising edge of EN or a falling edge of
  ENB**. Recovery = an EN cycle (low, then high) via the expander; that is the fault model's
  `EnCycle` action.
- Supply-voltage faults auto-recover when the voltage returns.
- With OCC low (its internal pull-down; the PCB is assumed not to drive it) the over-current
  detection (11 A typ, short to supply/ground/load) stays off until the EN cycle. With OCC high
  it retries every 500 ms on its own.
- The chopper current limiter (6.5 A typ, falling to 2.5 A above ~160 °C junction) is separate,
  does not touch DIAG, and just limits current; a stalled Pololu 25D motor (~5.5 A) sits under it.

## PWM input limits (Table 10.1)

- Maximum PWM frequency **20 kHz**.
- **Both the high and the low portion of PWM1/PWM2 must be ≥ 10 µs** (Note 1). Input delay
  tD(on)/tD(off) is 8 µs typ, 13 µs max.
- Therefore at 20 kHz only 20–80 % duty is within spec; at 10 kHz, 10–90 %. v4 uses **10 kHz,
  10-bit**, and `Esp32Hal` keeps drive duty inside **12–88 %** (12 µs pulses, clear of the
  13 µs input delay) except for the two steady levels: 0 (brake) and 100 % (DC forward/reverse).
  Requests between 88 % and 100 % are written as 88 %; the profile ceilings (75 % for the 19:1,
  100 % elsewhere) and the 12 % rescue jog floor fit this band. 10 kHz is at the edge of
  audibility; it is a deliberate trade against the datasheet limits.
- Enable/disable propagation tDEN ≤ 5 µs; output delay ≤ 13 µs; slew 0.3–1.6 µs.

## Other

- Operating VBAT 4.5–28 V (absolute max 28 V DC, 40 V for 0.5 s). Back-EMF into a supply that
  cannot sink current can push VBAT up; the datasheet asks for this to be verified per design.
- OCM (high-side current monitor, 0.223 % of load current into 220 Ω) is not wired to the ESP32
  on this PCB; if a future board revision adds it, stall detection by current becomes possible.
- Regeneration (7.9): before any High-Z transition the IC briefly recirculates coil current
  through the low side; nothing for the firmware to do.
