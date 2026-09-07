# Upstreaming status board

Tracks the effort to contribute R2 Uppity Spinner ALT improvements back to the upstream
projects, so any session can pick up where the last left off. **Last updated: 2026-08-16.**

Read the hard rules in [`../CLAUDE.md`](../CLAUDE.md) first — public PRs and pushes need
explicit per-action approval, and nothing here justifies editing local production code
(the sketch working tree or `~/Documents/Arduino/libraries/Reeltwo/`) in place. Do
upstream work in a throwaway clone under the scratchpad or `/tmp`.

Context: `thePunderWoman` now maintains the reeltwo GitHub org and did her own ESP32/IDF5
compat sweep in July 2026. She explicitly invited these contributions and asked to be
pinged on the PRs. House style: keep both code paths behind version guards, one file per
PR, honest test plans.

---

## Done ✅

| PR | Repo | What | Merged |
|---|---|---|---|
| [#1](https://github.com/travisccook/droidnet-command-library/pull/1) | travisccook/droidnet-command-library | ALT board file (`r2uppityspinner-alt.json`, 41 commands), lib v2.1.0→2.2.0 | 2026-07-06 |
| [#20](https://github.com/reeltwo/Reeltwo/pull/20) | reeltwo/Reeltwo | ESP-NOW recv/send callback signatures on ESP-IDF 5.x (`ReelTwoSMQ32.h`) | merged |
| [#21](https://github.com/reeltwo/Reeltwo/pull/21) | reeltwo/Reeltwo | iOS "Incorrect Password" softAP / WPA2-PSK fix (`wifi/WifiAccess.h`) | merged |

Also relevant: her [reeltwo/R2UppitySpinnerV3#6](https://github.com/reeltwo/R2UppitySpinnerV3/pull/6)
(mid-chain serial buffer corruption) merged 2026-07-16. Her library PRs #18 (PSController,
PWMDecoder) and #19 (ServoDispatchPrivate) landed the other half of the local ESP32 patch set.

---

## In flight / to do 🔜

### 1. `core/AnalogWrite.h` LEDC fix → reeltwo/Reeltwo (needs rework before PR)
The local patch calls `ledcAttach` / pin-based `ledcChangeFrequency` **unconditionally**,
which breaks pre-3.0 cores — can't land as-is against the maintainer's dual-path house
style. Rework needed:
- Wrap in `#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)` with a full `#else`
  legacy branch (mirror her `ServoDispatchPrivate.h` in #19).
- Use `ledcAttachChannel(pin, freq, res, channelNum)` to **preserve the explicit channel
  identity** that `AnalogWrite` tracks in `_analog_write_channels[i]` — not plain
  `ledcAttach` (which auto-assigns).
- Then one-file PR off `master`, ping her.

### 2. Sketch ESTOP fast-path → reeltwo/R2UppitySpinnerV3 (real port, not cherry-pick)
The ALT moves ESTOP detection into the UART RX callback (`advanceEstopFsm`) ahead of the
FIFO/buffer, so a `:PX` arriving mid-chain is still caught. This is directly relevant to
her merged #6 (which drops mid-chain bytes) and is the strongest thing to offer. **Not a
cherry-pick:** ALT ingestion runs on software FIFOs (`sUsbFifo`/`sCmdFifo`); upstream reads
straight off `Serial.available()`. Needs a fresh, upstream-shaped implementation.

### 3. Sketch chained `:P*` fixes → reeltwo/R2UppitySpinnerV3
ALT separates the chain dispatcher's working buffer (`sCmdBuffer`) from the ingestion
buffer (`sBuffer`), fixing chains silently no-op'ing after the first command. Same
underlying "who owns `sBuffer`" problem as her #6, different angle. Port after / alongside #2.

### 4. (Local, optional) port her #6 `!sProcessing` gate into the ALT
Her fix gates serial ingestion on `!sProcessing`. The ALT's serial-triggered chain path
(dispatches from `sBuffer` while the two FIFO ingestion blocks also append to `sBuffer`)
has the same class of bug. Porting the gate is correct, and safe here because the ESTOP
FSM sits ahead of ingestion (dropped mid-chain bytes still trigger `:PX`). **This touches
production motion/serial code — treat as a careful, regression-reviewed change with the
owner's explicit sign-off, not a blind copy.**

---

## Not upstreaming ❌
ALT-specific and staying in the fork: aggressiveness levels, 19:1 lifter tuning, the
rebuilt web UI. Too build-specific to push upstream; don't offer them wholesale.

---

## Watch-outs
- **`PWMDecoder`:** upstream (her #18) used native `driver/rmt_rx.h`
  (`REELTWO_PWMDECODER_USE_RMT_RX_API`); the local patch used the Arduino RMT layer
  (`USE_RMT_DECODER`, `rmtReadAsync`). Dropping the local patch for hers is **not**
  like-for-like — **retest RC input on hardware** before trusting it.
- No local `arduino-cli`: Claude can't do clean matrix compiles. Verified-on-hardware
  claims come from the owner, not from CI.
