# Preamble detection, radio timing and currents of the FG25: measurements for the emulator

Measured on 2026-11-12 with four EFR32FG25 kits (BRD4271A, 868 MHz) running
Silicon Labs' RAILtest, EU mode 3. Purpose: calibrate the emulator's preamble
sniffing (emulator `docs/MEDIUM.md` §10), its radio timing (§9) and the power
profile values that were still estimates (emulator `docs/POWER_MODEL.md` §11).
Companion of [COCHANNEL_RESULTS.md](COCHANNEL_RESULTS.md) (same bench, same
RAILtest build).

Scripts: `tests/hw/sniff/` (preamble detection) and `tests/hw/radio_power/`
(timing and currents). Raw results (one JSON line per cycle, every console
line, the AEM dumps): `/home/yann/kit-runs/2026-11-12_em5_sniff/` and
`/home/yann/kit-runs/2026-11-12_em5_timing_power/`.

## 1. Summary

| Question | Measured (EU mode 3, 150 kbit/s) | Emulator parameter |
| --- | --- | --- |
| Time from the receiver being on (RAIL's RX state reached) to a preamble detection, preamble present | 252–293 µs, median 270 µs (254 cycles); the same after a TX (TX→RX) and from idle (idle→RX) | `--sniff-settle-us` 225 + one dwell |
| Time from the start of a preamble to its detection, receiver already on | 7–61 µs, median 47 µs (about 7 bits), in clusters near 20, 35 and 48 µs (29 cycles, preamble starting 698 µs after the trigger) | `--sniff-dwell-bits` 7 (the median) |
| Preamble that must be left when the receiver comes on for the frame to be received | 100 % received with 423 µs or more (long preamble) or 438 µs or more (12-byte preamble), 0 % with 398 µs or less; 3 of 18 at 413 µs | detection must leave 22 preamble bits before the 16-bit SFD: `--sniff-margin-bits` 22 |
| Detection against a co-channel FSK interferer (SIR) | first try: 100 % from +5.4 dB, 67 % at +3.9 dB, 0 % at +0.6 dB and below; within 4 ms: 100 % at +3.9 dB, 14 % at +0.6 dB, none at −2.8 dB; misses are not independent from try to try (§4.3) | `--sniff-curve` table |
| idle→RX, idle→TX, TX→RX | RAIL holds these state timings to a few µs from about 125–130 µs up (300 → 1000 µs moves the times by 700 µs); the preamble detector then needs about 225 µs more after RX. RX→TX not measured (RAILtest's TX from RX goes through idle) | radio options `WSSIM_PHY_TX_DELAY_US`, `WSSIM_PHY_RX_DELAY_US` |
| Currents (3.32 V, chip, AEM) | EM0 with the radio idle 7.29 mA, EM1 4.75 mA, EM2 7–8 µA, RX listening 11.15 mA (EM0), receiving 0.2 mA below listening, TX 19.8 / 27.4 / 41.0 / 57.4 / 75.2 mA at −10.1 / 0 / +9.9 / +14 / +16.9 dBm (RAIL's reported levels) | `efr32fg25-soc.ini` |
| Transitions (AEM, 10 µs) | idle→RX 80 µs, 0.16 µC; RX→idle 40 µs, 0.045 µC; idle→TX 100 µs, 0.37–0.40 µC; TX→idle 50 µs, 0.17–0.22 µC (above the idle current; the range is where the ramp is cut) | `[radio_transitions]` |

## 2. Setup

- **Kits:** R (receiver) 440240046, W (wanted) 440235509, I (interferer)
  440290182, as in the co-channel campaign; currents on 440290182 (BRD4002A,
  AEM at 100 kHz) and, as a check, 440235509 (BRD4001A, 10 kHz). The fourth
  kit, 440235475 (BRD4270B), was flashed but not used. All four were flashed
  with the RAILtest 2.19.3 mode-3 image of the co-channel campaign
  (`/home/yann/kit-runs/2026-11-09_cochannel/railtest_build/mode3/`), built by
  `tests/hw/cochannel/build_railtest.sh`.
- **PHY:** `PHY_WISUN_FAN_1v1_863MHz_Plan33_2FSK_3_EU` (150 kbit/s, 12-byte
  preamble, 2-byte SFD, SHR 747 µs), channel 0, SUN FSK PHR, 4-byte FCS.
- **Levels:** W at R about −21 dBm (pW +3 dBm) or −36 dBm (pW −15 dBm), far
  above sensitivity. Levels near sensitivity are not reachable on the desk
  (about −35 dBm at R at the lowest TX power): noise plays no part.
- **Lab traffic:** the MTS bench's Wi-SUN network reaches R at −63 … −85 dBm
  on the same channels, 25 dB or more below our frames; cycles whose trigger
  W or I did not receive are discarded, as in the co-channel campaign.

## 3. Method

### 3.1 Receiver on at a set point of a frame (`sniff.py`)

Each cycle, R sends a 10-byte trigger and turns its receiver on X µs after
the trigger's end: RAIL's TX→RX state time (`setTimings`, txToRx = X), so the
moment is set by R's hardware. W answers the trigger with `txAfterRx 1000`
and a 50-byte frame whose preamble is 960 bits (6.4 ms, the alternate
preamble length) or the PHY's own 96 bits. W's first preamble bit reaches R
698 µs after the trigger's end: from W's sync-word end at R (the frame's
timestamp, 7204 µs with the long preamble, 1444 µs with the short one) minus
the SHR. R prints the RAIL events RX_PREAMBLE_DETECT and RX_SYNC1_DETECT
with its timestamps, and its counters give the frames received per point
(`status`). The SYNC1 event is printed 150 µs before the frame's sync-end
timestamp (calibrated on frames with both); the analysis uses the frame
timestamps.

On a long preamble the radio detects again about every 1013 µs (detection,
then the 800 µs sync-word timeout of this PHY, then a new search); RAILtest's
event queue holds five events, so with more than four detections in a cycle
the later events and the frame's report are dropped (`missedRailAppEvents`).
Only the first detection after X is used; reception is counted from R's
counters.

That RAIL keeps txToRx: the first detection is 270 µs after X whatever X
(1500 to 5500 µs, table in §4.1), and the idle→RX and TX settings behave the
same way (§5).

### 3.2 Interferer (`sniff.py --pI`)

I sends a 250-byte frame (14.3 ms) starting with W's, so that R, on from
X = 3000 µs, sees W's preamble under I's payload (modulated, whitened FSK),
with 4.1 ms of W's preamble left. W at +3 dBm, I from −15 to +15 dBm. The
SIR is W's mean packet RSSI at R minus I's, each sent alone with R on before
the frame (`s3_levels`, 7–8 frames each). RSSI is reported in whole dB; the
SIR axis is accurate to about 1 dB, as in the co-channel campaign.

### 3.3 Timing (`timing.py`)

W sends a frame with a 30 000-bit preamble (200 ms); during it, R goes from
idle to RX (`rx 1`), and the time from the command's RAIL timestamp to the
first detection is recorded, for idleToRx = 100, 300 and 1000 µs. On W
alone: `tx 1` from idle and from RX, the time from the command's timestamp
to RAIL's TX start (`lastTxStart`), for idleToTx and rxToTx = 100, 300 and
1000 µs. 15 cycles per setting.

### 3.4 Currents (`power.py`)

The kit holds one state while `commander aem dump` records it (1 s on
440290182 at 100 kHz, 2 s on 440235509 at 10 kHz): RAILtest idle (radio
idle, MCU in RAILtest's main loop, EM0), RX on an empty channel, `sleep 1`
(EM1), `sleep 2` (EM2), a continuous PN9 stream (`setTxStream 1`) at several
powers. Shapes: 34-byte frames every 20 ms from idle at +9.9 dBm (45 frames)
for idle→TX and TX→idle; RAIL's RX duty cycle (2 ms RX, 10 ms idle, 83
windows) for idle→RX and RX→idle; W sending 250-byte frames to the measured
kit for RX while receiving.

## 4. Results: preamble detection

### 4.1 Detection time after the receiver is on

`s1_settle`, 960-bit preamble, receiver on at X (30 cycles per point; delay =
first detection minus X):

| X (µs) | Preamble left at X (µs) | Detected | Delay min / median / max (µs) | Received |
| --- | --- | --- | --- | --- |
| 100 (before the frame) | — | 29/29 | 605 / 645 / 659 after X, i.e. 7–61 µs (median 47) after the preamble's start | 29 |
| 1500 | 5598 | 25/25 | 273 / 274 / 288 | 25 |
| 2000 | 5098 | 26/26 | 252 / 269 / 293 | 26 |
| 2500–5500 (7 points) | 4598–1598 | 203/203 | 252 / 270 / 293 | 203 |

The 12-byte preamble gives the same (`s2_short`, 20 cycles per point): with
the receiver on 250 µs or more before the frame, detection 47 µs after the
preamble's start; on later, 239–298 µs after X.

So the receiver detects a preamble about **270 µs after it is on** (spread
±25 µs), and **7–61 µs after a preamble starts** when it was already on
(median 47 µs, 7 bits; the values cluster near 20, 35 and 48 µs). In the emulator's model (§10.2 of MEDIUM.md): settling 225 µs, then
dwells of 7 bits.

### 4.2 Preamble needed for reception

`s2_remain` (960-bit preamble) and `s2_short` (96 bits), receiver on at X,
20 cycles per point:

| Preamble left at X (µs) | 960-bit preamble: received | 96-bit preamble: received |
| --- | --- | --- |
| ≥ 448 | 100 % (every point) | 100 % (every point) |
| 438 | — | 19/19 |
| 423 | 17/17 | — |
| 413 | — | 3/18 |
| 398 | 0/20 | — |
| 388 and less | 0 (every point) | 0 (every point) |

Below the step no preamble detection is reported at all, although the
detector would still have 100–150 µs of preamble after its usual 270 µs:
the radio needs about 150 µs (22 bits) of preamble after the detection to
acquire timing and then the 16-bit SFD. The 50 % point is at about 418 µs of
preamble left, i.e. 270 µs + 22 bits. In the model: a detection must end at
least 22 + 16 = 38 bits before the SHR's end.

### 4.3 Detection against a co-channel interferer

`s3_sir` (receiver on at 3000 µs, 4.1 ms of W's preamble left, I's payload
over it), 24–29 cycles per point:

| SIR (dB) | +15.4, +15.0, +14.0, +12.0, +9.6, +7.9 (6 points) | +5.4 | +3.9 | +0.6 | −2.8 | −8.0 |
| --- | --- | --- | --- | --- | --- | --- |
| Detected within 300 µs of X (first try) | 100 % | 27/27 | 18/27 | 0/29 | 0/29 | 0/27 |
| Detected at all (within 4.1 ms) | 100 % | 27/27 | 27/27 | 4/29 | 0/29 | 0/27 |
| W's frame received (sync) | 100 % | 26/27 | 13/27 | 0/29 | 0/29 | 0/27 |

At +3.9 dB the late detections came 306–881 µs after X, one to twelve
dwells later; of the 9 cycles that missed the first try, 5 needed 7–13 more
dwells. The tries are therefore not independent: for a given alignment of
the interferer's bits the detector tends to keep missing. The emulator draws
each try independently, which is optimistic for detection over several
dwells at a marginal SIR. At +0.6 dB four detections in 29 cycles of about 80 dwells
each: about 0.002 per dwell. Per try (a 7-bit dwell): 1 from +5.4 dB, about
0.65 at +3.9 dB, about 0.002 at +0.6 dB, 0 at −2.8 dB. The reception column
follows the medium's interference curve for a 50-byte frame (50 % at about
+3.7 dB, COCHANNEL_RESULTS.md §4.1).

## 5. Results: timing

| Path | Setting (µs) | Measured | Cycles |
| --- | --- | --- | --- |
| idle→RX, command to first detection | idleToRx 100 / 300 / 1000 | 385–406 / 542–591 / 1251–1287 µs | 15 each |
| idle→TX, command to TX start | idleToTx 100 / 300 / 1000 | 3679–3803 / 3850–3967 / 4550–4663 µs | 15 each |
| RX→TX, command to TX start | rxToTx 100 / 300 / 1000 | 3694–3814 µs whatever the setting: not measured (see below) | 15 each |
| TX→RX, end of TX to first detection | txToRx 1500 … 5500 | X + 252–293 µs | §4.1 |

- From 300 to 1000 µs the measured times move by 700 µs to within a few µs:
  RAIL holds its state timings exactly when they are longer than its own
  minimum. At 100 µs they do not (idle→RX: 175 µs less than from 300 µs, not
  200), the radio's own start-up being longer than 100 µs.
- After idle→RX, as after TX→RX, the first detection comes about 270 µs after
  the RX state is reached (1000 µs setting: 1270 µs after the command).
- RAILtest's `tx` command spends about 3.6 ms before RAIL's TX start (FIFO
  load and printing), so the absolute TX start latency from idle cannot be
  read from it; only the setting's effect can. From RX, RAILtest's TX does
  not use rxToTx (constant time): it idles the radio first.
- RAIL's defaults in RAILtest: idleToRx 120, txToRx 100, idleToTx 120,
  rxToTx 130 µs; our firmware sets 100, 182, 100, 192 µs
  (`drivers/rail/rail_phy.c`).
- **Not measured:** the channel change in RX and the CCA duration (the
  host-timed channel changes of `power.py --states chan` mix RAILtest's
  command handling into the trace; the firmware's CCA, 8 symbols, was not in
  the loop). The AEM shapes of §6.2 give the transitions' durations.

## 6. Results: currents

### 6.1 Steady states

Mean of the AEM samples, supply 3.32 V. Kit A 440290182 (100 kHz, 1 s), kit
B 440235509 (10 kHz, 2 s):

| State | Kit A (mA) | Kit B (mA) |
| --- | --- | --- |
| EM0, radio idle (RAILtest main loop) | 7.29 | 6.92 |
| EM1, radio idle (`sleep 1`) | 4.75 | 4.42 |
| EM2, radio off (`sleep 2`) | 0.008 (median 0.002) | 0.007 |
| RX listening, empty channel (EM0) | 11.15 | 10.72 |
| RX receiving (EM0), 250-byte frames at −43 dBm (RSSI at the kit) | 10.9 (0.2 below listening) | — |
| TX −10.1 dBm (PN9 stream, EM0) | 19.78 | 19.82 |
| TX 0 dBm | 27.42 | 27.01 |
| TX +9.9 dBm | 40.98 | 39.92 |
| TX +14.0 dBm | 57.39 | 56.19 |
| TX +16.9 dBm (`setPower 200` gives 169 deci-dBm) | 75.20 | 74.10 |

- The TX levels are RAIL's reported values for RAILtest's PA configuration (`setPower` answers with the level it set; 200 deci-dBm gives 169), not RF measurements; our firmware's PA configuration was not checked.
- Radio increments over EM0 with the radio idle: RX 3.86 / 3.80 mA; TX 12.5,
  20.1, 33.7, 50.1, 67.9 mA (kit A; kit B within 0.8 mA). The two kits agree
  within 3 % on every increment; their bases differ by 0.3–0.4 mA.
- Each sample's spread (5th–95th percentile) is ±0.15–0.25 mA, the means of
  1 s are stable to 0.01 mA; the AEM's own accuracy is the uncertainty (about
  1 %, and the BRD4001A reads lower than the BRD4002A here).
- EM1 here runs on RAILtest's clocks (SYSCLK from the RF PLL, as in our
  firmware); it is 2.6 mA above the datasheet's EM1 at 97.5 MHz (2.145 mA).
  RX listening with the MCU in EM1 is therefore about 4.75 + 3.86 = 8.6 mA,
  against 8.15 mA measured on our firmware in HW5.
- **RX while receiving** (`rxbusy`, run `power_440290182_gap20`, `RXBUSY_GAP_MS=20`, W at −15 dBm, RSSI −43 dBm at the kit): with frames every 35 ms, the kit draws
  11.1 mA listening, 10.9 mA during each 14.3 ms frame, then 9.3 mA for
  10.5 ms while RAILtest prints the packet (CPU waiting on the UART). So
  receiving costs 0.2 mA less than listening.
- **TX at +9.9 dBm** in frames (`txpkt`) has the same plateau as the stream
  (41.1 mA against 41.0).

### 6.2 Transitions

Averaged edges, 10 µs samples, kit A; charge above the radio-idle EM0 level
(7.3 mA):

| Transition | Shape | Duration | Charge above idle |
| --- | --- | --- | --- |
| idle→TX (+9.9 dBm) | 8.0, then 9.5 mA for 60 µs (synthesizer), then a ramp 13–17 mA, then the plateau | about 100 µs | 0.37 µC |
| TX→idle | 35, 17, 11, 9.5, 9.0, 7.8 mA | about 50 µs | 0.17 µC |
| idle→RX (duty cycle) | 7.5, 8.4, then 9.5 mA for 40 µs, 10.2, 10.6, plateau 11.0 | about 80 µs | 0.16 µC |
| RX→idle | 10.2, 8.2, 7.7, 7.6, 7.4 mA | about 40 µs | 0.045 µC |

A 2000 µs RX window of the duty cycle lasts 2110–2160 µs above 9.2 mA.
Where a ramp starts and ends is a matter of threshold at 10 µs per sample:
idle→TX is 0.37–0.40 µC and TX→idle 0.17–0.22 µC depending on whether the
first and last partial samples are counted.

## 7. Parameters for the emulator

1. **Sniffing** (`docs/MEDIUM.md` §10.5): settling 225 µs, dwell 7 bits,
   margin 22 bits before the 16-bit SFD (a new setting,
   `--sniff-margin-bits`), curve `-2.8:0,0.6:0.002,3.9:0.65,5.4:1` (per try,
   against the SINR). With these, the model detects 272 µs after the receiver
   is on, 47 µs after a preamble starts, and acquires a frame when it was on
   418 µs or more before the preamble's end. The emulator applies the table with its
   empirical receiver curve; with its layered model (`--medium-rx-model=layered`)
   the default stays `ber:16` on that model's noise curve, which keeps its
   field margin (the kits measured detection against an interferer only).
2. **Turnaround:** our firmware configures RX→TX 192 µs and TX→RX 182 µs.
   The kits showed RAIL holding idleToTx, idleToRx and txToRx from about
   125–130 µs up; RX→TX was not measured (RAILtest's TX from RX goes
   through idle). After TX→RX the receiver detects 225 µs plus one dwell
   later, and receives a frame whose preamble still has about 418 µs left:
   with the 12-byte preamble (640 µs) every frame starting after the end of
   its own TX is received, so the emulator's RX delay (which loses every
   frame starting during it) should be `max(0, txToRx + 418 − preamble)`:
   0 µs in mode 3, 173 µs with an 8-byte preamble at 150 kbit/s.
3. **Power profile** (`efr32fg25-soc.ini`): TX per level from §6.1, the
   increments anchored to the +10 dBm value the firmware measured (HW5), the
   maximum at +16.9 dBm (RAIL's reported maximum for RAILtest's PA
   configuration); RX while receiving 0.2 mA below listening; the four
   transitions of §6.2. The MCU states (EM0 7.29, EM1 4.75 mA with the RF
   PLL) are recorded but the profile keeps its split (datasheet EM1 plus the
   radio's share), which matches the chip's totals in every FFN state; see
   the emulator's POWER_MODEL.md §12.10.

## 8. Limits

- EU mode 3 only. Mode 1a (50 kbit/s) was not measured; the bit counts
  (dwell 7, margin 22) are expected to carry over in bits, the settling time
  in µs, but this is not shown.
- Detection near sensitivity is not reachable on the desk: the curve is
  measured against a co-channel FSK interferer (SIR), and the emulator
  applies it to the SINR, noise included. Detection on noise alone, and
  false detections on noise, were not measured.
- RAILtest only: RAIL's preamble detection events as configured by the
  Radio Configurator for this PHY; our firmware's MAC and its RX duty-cycle
  settings were not in the loop. RAIL's RX duty cycle in preamble-sense mode
  was not used: the detection was timed with the TX→RX and idle→RX state
  times instead.
- The preamble start at R (698 µs) is derived from the frame's sync-end
  timestamp minus the SHR; the 47 µs detection time is accurate to about
  ±15 µs (the first bits come during the transmitter's PA ramp).
- Interferer points: one wanted level (−21 dBm), one receiver-on point,
  24–29 cycles per point; per-try probabilities have a binomial uncertainty
  of about ±0.1 at 0.65, and the tries are not independent (§4.3).
- The TX levels are RAIL's, not measured at RF (§6.1).
- Channel change time and CCA duration: not measured (§5).
- Currents: RAILtest's MCU states, not our firmware's; one PHY (mode 3,
  channel 0); TX on a PN9 stream; one sample kit per board type.

## 9. Reproducing

```bash
# flash RAILtest mode 3 (as in COCHANNEL_RESULTS.md §8) on R, W, I
tests/hw/sniff/run_mode3.sh <out>/mode3                       # §4 (about 40 min)
tests/hw/sniff/sniff.py s3_levels <out>/mode3 --lp 96 --x 100 --pW 30 \
    --pI -150,-110,-90,-70,-50,-30,0,30,60,100,150 --n 8 --alone 1   # SIR levels
tests/hw/sniff/analyze.py <out>/mode3 s2_remain --start 698   # tables
tests/hw/radio_power/timing.py <out>/timing --n 15            # §5 (about 3 min)
tests/hw/radio_power/power.py --sn 440290182 --peer 440235509 --out <out>/power_440290182   # §6
RXBUSY_GAP_MS=20 tests/hw/radio_power/power.py --sn 440290182 --peer 440235509 \
    --out <out>/power_440290182_gap20 --states rxbusy           # §6.1, receiving vs listening
```

The RX duty-cycle shape of §6.2 was recorded with RAILtest's
`configRxDutyCycle 1 2000 10000`, `enableRxDutyCycle 1`, `rx 1` and a 1 s
`commander aem dump` (`power_440290182/440290182_dutycycle.csv.gz`).

## 10. 300 kbit/s for the fire alarm mesh stack (2026-11-25)

RAILtest 2.19.3 with the stock PHY `PHY_WISUN_FAN_1v1_867MHz_Plan43_2FSK_5_SG` (300 kbit/s, h = 0.5, 866–869 MHz, within the kits' 868 MHz band), built with `tests/hw/cochannel/build_railtest.sh mode5`. Currents on 440235509 (BRD4001A, 10 kHz energy monitor), whole chip, MCU awake in RAILtest's loop.

| Quantity | Value |
| --- | --- |
| From `rx 1` to a preamble detection, preamble already on air, idleToRx 100 / 300 / 1000 µs | median 263 / 445 / 1144 µs (15 each, spread ±5 µs): detection about 160 µs after the receiver is ready, against about 270 µs at 150 kbit/s (§4.1) |
| EM2 | 6 µA |
| Idle (radio idle, MCU in EM0) | 6.83 mA |
| RX, empty channel | 10.71 mA (3.9 mA above idle) |
| TX at 10.8 dBm (the highest level reached when asking for 11 dBm) / 14.0 dBm, PN9 stream | 43.8 / 56.3 mA |
| Short receive windows by RAIL's RX duty cycle (`radio_power/dutycycle.py`): extra charge over idle, against the window (period 200 ms) | 150 µs: 10.8 µC; 300: 11.4; 500: 12.1; 1000: 14.0 µC: slope 3.7 mA, the RX current |
| The same, 300 µs windows at periods 50, 200, 400 ms | mean current above idle 0.081, 0.057, 0.052 mA: a fixed 1.66 µC per 300 µs window (about 0.5 µC beyond the RX current) plus a constant 48 µA while the duty-cycle mode runs |

What it means for the stack:
- **The radio's own cost of a short window is small:** about 0.5 µC to start and stop the receiver, plus 3.9 mA while it is on. The 7.35 µC per window of our Wi-SUN low-power firmware came mostly from its software teardown (the 1 ms `listen_off` of the LFN profile), which a dedicated firmware need not have. The 48 µA constant belongs to the duty-cycle mode running with the MCU awake; a firmware that sleeps in EM2 between windows replaces it by the 6 µA floor and the wake (0.19 µC, §6 of the LFN profile).
- **The window must open well before the preamble.** Detection comes about 160 µs after the receiver is ready, and at 150 kbit/s a frame was received only with about 420 µs of preamble left when the receiver came on (§4.2). The specification's preamble lasts 213 µs at 300 kbit/s and its window opens only 30 µs (plus the clock allowance) before the sync word, so on the FG25 a frame would be missed: the receiver must be on some 0.2–0.4 ms before the preamble starts (to be measured at 300 kbit/s with `tests/hw/sniff/`), which makes each window about 0.4–0.6 ms long.
- **Estimate per listening moment** (to be confirmed with an EM2 firmware): wake 0.19 µC + radio 0.5 µC + 0.5 ms × (3.9 mA radio + about 2.1 mA MCU in EM1) ≈ 3.7 µC; five per second ≈ 18 µA, plus the 6 µA floor: about 24 µA for listening alone, against 16.5 µA in the ns-3 parity set and 58 µA with the LFN profile.
- Emulator settings: superseded by §11 (measured with the dedicated firmware).

## 11. Dedicated measurement firmware at 300 kbit/s (2026-11-25)

`tests/hw/fmmeas/` holds a small bare-metal firmware (internal measuring equipment) with the fire alarm mesh specification's modulation and frame timing: 300 kbit/s 2-FSK (deviation 75 kHz) at 863.5 MHz, an 8-byte preamble, the 16-bit sync word 0xA73E, a CRC-32 (IEEE 802.3) and a fixed 16-byte payload. The framing is the radio's, not the specification's: the hardware appends the CRC after the payload and computes it over the payload only, and the length is fixed. The air time is the same (30 bytes, 800 µs), so the figures hold; a hardware port of the stack will need its own CRC and length handling. Between receive windows the chip is in deep sleep (EM2); windows and transmissions are scheduled on the radio's clock. `run.py` builds each variant, flashes it and measures; `build.sh` builds one.

| Quantity | Value |
| --- | --- |
| Floor (EM2, no radio) | 5.8 µA |
| Charge of one listening moment, deep sleep to deep sleep, against the window's length (five per second) | 100 µs: 5.3 µC; 200: 5.7; 300: 7.2; 400: 8.8; 600: 9.5 µC; fitted: a fixed 4.3 ± 0.5 µC plus 9.3 ± 1.5 mA (1 σ; one 6 s record per length, residuals ±0.5–0.8 µC) while the receiver is on. The slope is poorly determined (about 6 mA would be expected: 3.9 mA radio + 2.1 mA processor); it matters for long windows only |
| Listening alone, 300 µs windows, five per second | 42 µA |
| When a frame is received (one kit sending back-to-back frames, the other opening windows) | when its preamble starts between about 120 µs before the window opens and 297 µs before it ends (300 µs windows: 35 of 301 windows caught a frame, as the 120 µs width predicts; 600 µs windows: 420 µs width). The radio reported the frame's end instead of its preamble start; the offsets were corrected by the frame's 800 µs. Correction (§12.1): the sender did not send every 1013 µs but every third or fourth slot (a scheduling error in the firmware, since fixed), so the agreement of the 35 catches with the 120 µs width was fortuitous; the edges stand and were confirmed |

What it means:
- **The window must open at least about 93 µs before the sync word** (120 µs before the end of the 213 µs preamble), not the 30 µs of the specification's allowance, and stay open about 84 µs past it. With the stack's `--rx-lead-us 80` and the emulator's detection set to "28 bits of preamble left at the opening" (`--sniff-settle-us=0 --sniff-dwell-bits=7 --sniff-margin-bits=21`), seven emulated detectors deliver every status from the first cycle at 43–49 µA (with the 4.3 µC fixed cost of the console build); without the earlier opening, formation takes three cycles and costs more.
- **The timestamp's position, assumed here, is verified in §12.1:** the radio's frame-end timestamp includes the CRC, so the 800 µs correction and the 93 µs stand (§12.1 gives 88–97 µs).
- **The window's end is a firmware choice.** The measured windows extend themselves only once the sync word is found (`hard_window_end = 0`); the specification extends on preamble detection. A frame whose sync word starts less than about 84 µs before the window's planned end is lost on this firmware. The emulator's FG25 settings model only the opening edge, so with this firmware hardware could lose late beacons that the emulator delivers.
- **The fixed 4.3 µC per listening moment dominates** (21.5 µA at five moments per second; explained and reduced to 2.4 µC in §12.2): it is the wake from EM2, the high-frequency crystal's start and the radio's start and stop. Reducing it (the crystal's start-up time, the radio's wake path, a lighter sleep state at the window) is the main lever towards 31 µA. The radio's own currents are as measured with RAILtest (§10).
- **Where the fixed cost goes.** A variant that only wakes the processor five times per second (sleep timer, no radio) costs 2.85 µC per wake: about 1.1 ms at 2.6 mA, whereas our Wi-SUN low-power firmware's timer wakes cost about 0.1–0.2 µC. Running the processor from its RC oscillator instead of the radio's PLL changes nothing (2.73 µC). So about 2.8 µC of the 4.3 µC is this firmware's wake from deep sleep, and about 1.5 µC the radio's start and stop. Why a wake lasts 1.1 ms here (the crystal's start-up wait, leaving deep sleep, the console's serial port) is the next item; a wake as short as the Wi-SUN firmware's would bring a listening moment to about 2 µC and listening alone to about 16 µA.
- **Settings tried for a cheaper wake** (`fmmeas_*.slcp`): without the serial console (`fmmeas_noio.slcp`) a 300 µs listening moment costs 5.6 µC instead of 7.2 µC, so listening alone costs 33.8 µA; the bare wake stays at 2.6 µC. Adding fast voltage scaling on wake-up and no debugging in deep sleep (`fmmeas_lp.slcp`) lowers the floor to 5.3 µA but does not shorten the wake (3.2 µC, windows 5.7–10.0 µC). The emulator profile uses the console-free figure: a fixed 2.8 µC plus 9.3 mA.
- The receive-to-transmit turnaround, the 0.1 ms carrier sense and the clocks' drift are measured in §12.

## 12. Timestamps, wake cost, turnaround, carrier sense and clocks at 300 kbit/s (2026-11-25)

The firmware of §11 (`tests/hw/fmmeas/`), extended: more roles (continuous receiver, responder, initiator, interferer, wake only with options), the radio's timestamps and software event times on the console, RAIL's timer kept across deep sleep (`FM_TSYNC`), a shorter early wake of the power manager (`FM_PMOVH`), extra build settings (`build.sh`: `SLCP`, `CONF`, `RADIOCONF`; `radio_settings_nocrc.radioconf` is the same PHY without CRC). `fmlog.py` captures the consoles and analyses the logs and the energy-monitor dumps; `run.py` runs the measurements below (`app.c` gives the console formats). Kits: currents on 440235509 (BRD4001A, energy monitor at 10 kHz), timing between 440235475 and 440240046, side by side, +10 dBm. Charges are per wake: the samples of each wake above the floor, the floor being the mean of the samples outside any activity; the median over the 30 wakes of a 6 s record (spread between wakes 0.01–0.07 µC).

Firmware correction: the slot arithmetic of the sender and of the windows wrapped with RAIL's 32-bit clock, so the "back-to-back" sender of §11 sent in every third or fourth slot of 1013 µs (spacing 3039 or 4052 µs, measured). Fixed (`FM_GUARD_US`); a sender at 1013 µs slots kept on the crystal now sends every second or fourth slot (2026 or 4052 µs), the radio needing more than the 213 µs left between frames. The windows of §11 were regular (five per second).

### 12.1 Where the receive timestamp sits

| Quantity | Value |
| --- | --- |
| RAIL's default receive timestamp at this PHY | position "frame end" (`SL_RAIL_PACKET_TIME_AT_PACKET_END`), and RAIL's on-air duration from the end of the sync word (`packet_duration_us`, EFR32xG25 only) is 533 µs = 20 bytes: payload and CRC (426 µs = 16 bytes without CRC), on every frame |
| Software time of the sync-word event (`SL_RAIL_EVENT_RX_SYNC_0_DETECT`) minus the frame-end timestamp | −486 to −499 µs with CRC (by run; several thousand frames), −380 to −391 µs without CRC; in runs with the same sender, 106–107 µs apart: the 4 CRC bytes. The event cannot come before the sync word has ended, so the end mark cannot be the payload's end (the event would then come 60–70 µs before the sync word's end); with the end mark at the CRC's end, the event comes 36–46 µs after it |
| Reception interval, by the same windows with and without CRC (60 s each, one kit sending, the other opening 600 µs windows every 200 ms) | frame-end timestamp against the window's opening: first and last frames caught at 684 and 1102 µs with CRC, 585 and 993 µs without; the edges move by 99 and 109 µs (107 expected if the mark includes the CRC, 0 if not) |
| Delay of the frame-end timestamp after the frame's last bit (round trip, §12.3) | 3.5–10.5 µs (depends on how the transmit timestamps sit, see §12.3) |
| Preamble start of the frames caught, against the window's opening (timestamp − 800 µs) | 300 µs windows: −118 to +2 µs (45 frames); 600 µs windows: −116 to +302 µs (60 frames), and −116 to +285 µs with RAIL's timer kept across deep sleep and the shorter early wake of §12.2 (60 frames). The early edge is sharp: in 20 µs bins the count is already full in the bin next to it (285 frames pooled) |

What it means for the stack:
- **The window must open at least 97 µs before the sync word starts** (the end of the 213 µs preamble), that is no later than 116 µs after the preamble's first bit. With the timestamp delay of 3.5–10.5 µs the true figure is 86–93 µs; 97 µs is the bound that holds whatever the delay. Three runs (300 and 600 µs windows, with and without RAIL's timer sync and the shorter early wake of §12.2) agree within 2 µs on the early edge; the sampling of the edge adds at most about 7 µs. The 93 µs of §11 stands.
- With the specification's window opening 30 µs before the sync word, the receiver must come on **67 µs earlier**: the stack's `--rx-lead-us 80` covers it with at least 13 µs to spare. The emulator's "28 bits of preamble left at the opening" (93 µs) matches.
- **Late edge:** a frame is lost when its sync word ends less than about 31 µs before the window's planned end (300 µs window: preamble starting at most 2 µs after the opening; 600 µs: at most 302 µs); the window then extends itself. The stack's window must therefore cover the sync word's end plus about 31 µs (34–41 µs counting the timestamp's delay).
- The frame-end timestamp is the radio's best reference: subtract 800 µs plus 4–10 µs for the preamble's start, or 533 µs plus the same for the sync word's end.

### 12.2 The fixed cost of a listening moment

The current traces (10 kHz) show the parts of a wake. Default settings (`fmmeas_noio.slcp`, 300 µs window): the crystal starting (about 0.25 ms at 0.64 mA), then about 0.5 ms at 3.9 mA with nothing to do, then the receiver (about 0.45 ms at 6.2 mA), then the end (about 0.1 ms).

The idle 0.5 ms is the power manager's early wake. Before any timer that needs the high-frequency crystal (the radio's always do), it wakes the chip early by the crystal's start-up time, as the HFXO manager estimates it, times 1.125, plus 95 µs. On these kits the estimate stays at its default of 600 µs (20 sleep-timer ticks, read on the kit) and is never updated, while the crystal starts in about 0.25–0.3 ms; the chip then waits about 0.45 ms in EM1 at 3.9 mA (processor on the 97.5 MHz radio PLL, crystal and radio clocks on). Each tick (30.5 µs) removed from the early wake saves about 0.12 µC on a bare wake; on a listening moment the saving is smaller without RAIL's timer kept (0.05 µC per tick) than with it (1.5 µC for 14 ticks). A sleep-timer wake that does not need the crystal costs 0.76 µC here; the 0.1 µC timer wakes of the Wi-SUN low-power firmware were not reproduced and not investigated further, since every wake of the stack needs the crystal. A wake-only image that never sets the radio up has a floor of 14 µA (RAIL initialised but never used); this does not concern a firmware that uses the radio.

| Settings (each one 6 s record, five wakes per second) | Wake only (`FM_ROLE 3`) | 300 µs listening moment | Floor |
| --- | --- | --- | --- |
| No console (`fmmeas_noio.slcp`), default | 2.54 µC | 5.58 µC | 5.7 µA |
| the same, sleep timer flagged as not needing the crystal | 0.76 µC (0.4 ms at 1.3–2 mA) | — | — |
| the same, RAIL's timer kept across deep sleep (`FM_TSYNC 1`) | — | 5.61 µC | 5.8 µA |
| early wake 8 / 10 / 12 / 14 ticks shorter (`FM_PMOVH`), RAIL's timer not kept | 1.60 µC (8) | 5.23 / 5.13 / 5.04 / 4.95 µC | 5.8 µA |
| RAIL's timer kept and early wake 14 ticks shorter | — | 4.10 µC (repeated: 4.11) | 5.7 µA |
| the same, no debugging in deep sleep (`SL_POWER_MANAGER_INIT_EMU_EM2_DEBUG_ENABLE 0`) | — | 4.10 µC (14 ticks), 4.20 µC (12), 4.44 µC (10) | 5.1–5.3 µA |
| early wake 15 ticks shorter, RAIL's timer not kept | 0.77 µC | no window at all (the firmware stopped opening windows; cause not examined) | — |
| processor on the 39 MHz crystal or on the 19 MHz RC oscillator instead of the radio PLL | 2.18 / 2.16 µC | 7.5 / 7.6 µC (0.8 ms more at 2.4 mA before the receiver starts) | 5.8 µA |
| fast voltage scaling on wake-up and no debugging in deep sleep (`fmmeas_lp.slcp`), RAIL's timer kept | 2.20 µC | 5.49 µC; 4.07 µC with 12 ticks shorter; no window with 14 | 10.6 µA once the radio has run (5.2 µA before) |

**Recommended settings for a low-power firmware** (measured together): no serial console; no debugging in deep sleep; the processor on the radio PLL (the default); RAIL's timer kept across deep sleep (`sl_rail_config_sleep` with `SL_RAIL_TIMER_SYNC_DEFAULT`, needed anyway for the timing, §12.4); the power manager's early wake 12 ticks (366 µs) shorter (`sl_power_manager_schedule_wakeup_set_restore_overhead_tick(-12)`), 3 ticks (92 µs) away from the setting at which the windows fail. With these settings:

| Window | 100 µs | 200 µs | 300 µs | 600 µs |
| --- | --- | --- | --- | --- |
| Charge of a listening moment | 3.08 µC | 3.60 µC | 4.20 µC | 6.07 µC |

A line fits within ±0.05 µC: **a fixed 2.43 ± 0.05 µC plus 6.03 ± 0.1 mA while the window is open** (the chip's whole current with the receiver on: 3.9 mA radio, the rest processor and clocks). Floor 5.2 µA. Listening alone, 300 µs windows five times per second: 26.3 µA measured (33.8 µA with the default settings, 42 µA in §11 with the console).

What it means:
- The fixed cost drops from about 3.8 µC (default settings without console: 5.58 µC minus 300 µs at 6.03 mA) to 2.4 µC. About 0.15–0.2 µC of it is the crystal's start; the rest is the processor's restart and the radio's start and stop, which the 10 kHz trace does not separate further. The remaining lever is the window's length: 0.6 µC per 100 µs.
- The early wake is a margin against the crystal's start-up time, which varies with temperature and ageing; 12 ticks shorter keeps a margin, 15 ticks shorter fails. A product firmware should measure the start-up time (or have the HFXO manager do it) and set the margin from it, not use a fixed number. **Not verified:** the crystal's start-up over temperature.
- RAIL's timer kept across deep sleep costs nothing measurable and is required (§12.4).

### 12.3 Receive-to-transmit turnaround and carrier sense

Responder (`FM_ROLE 5`, 440235475): each frame of the initiator (`FM_ROLE 6`, 440240046, one frame every 20 ms) is answered by a transmit scheduled at the frame-end timestamp E plus a delay D, from the frame-received callback. Both kits log their timestamps; 200–400 exchanges per setting.

| Quantity | Value |
| --- | --- |
| Frame-received callback after E | 58–69 µs |
| Answer's preamble (its own transmit timestamp) after the scheduled time | 13 µs, constant (D = 220, 240, 250, 300, 400 µs) |
| Shortest delay D accepted from the callback | 220 µs; at 200 µs RAIL refuses the transmit (`SL_RAIL_STATUS_INVALID_PARAMETER`) |
| Answer's end at the initiator, after the end of its own frame (both its timestamps) | D + 827 µs (± 4 µs) |
| Transmit timestamps: end minus preamble start | 807 µs (the frame is 800 µs) |
| Single carrier sense (`sl_rail_start_scheduled_cca_csma_tx`, one try, no backoff, −75 dBm) | 100 µs: the preamble starts 125 µs after the scheduled time; 200 µs: 231–232 µs. From the start of the carrier sense to the first bit: its length plus 25–32 µs |
| Carrier sense, quiet channel (100, 150, 200, 300 µs; 500 tries each) | always clear |
| Carrier sense, continuous PN9 interferer on the channel (third kit, a few cm away) | 604 of 604 busy |
| Carrier sense against a third kit's frames | detected: half the tries busy while that kit's 20 ms frames overlapped every second try |

The round trip gives 2d − a − b = 14 µs, where d is the receive timestamp's delay after the last bit, a the transmit end timestamp's delay after the last bit and b the transmit preamble timestamp's delay after the first bit. If the transmit end timestamp is exact (the 7 µs beyond 800 µs being part of RAIL's duration), d = 3.5 µs; if the preamble timestamp is exact, d = 10.5 µs.

What it means for the stack:
- **The 0.4 ms answer is feasible with margin:** scheduling from the callback works down to 220 µs after the timestamp. For the preamble to start 400 µs after the received frame's last bit, schedule at E + 400 − 13 − d ≈ E + 380 µs.
- **A 0.1 ms carrier sense works at this PHY:** it detects a strong signal reliably and gives no false busy on a quiet channel. With it, the first bit comes 125 µs after the scheduled time: schedule at about E + 270 µs for a preamble at 400 µs. **Not verified:** the threshold's accuracy near −75 dBm (the kits were only tested with strong signals; no attenuators).

### 12.4 Clocks: the crystals, the sleep clocks and RAIL's time across deep sleep

One kit sends about every 100 ms, the other receives continuously (its radio clock then runs on its 39 MHz crystal) and timestamps the frames; a line fitted to the arrival times gives the sender's clock against the receiver's crystal (60–120 s per run, 650–1200 frames, single arrivals within 0.7–2.3 µs rms of the line). The sender keeps time three ways: its radio clock on its crystal (`FM_EM1`); its sleep timer alone (`FM_ROLE 9`, the 32.768 kHz crystal through the sleep timer, a frame sent at once after each wake); its radio clock kept across deep sleep by RAIL (`FM_TSYNC`). The receiver also reads its own sleep timer against its radio clock. Figures in ppm against the crystal of kit 440235475; negative: slower.

| Kit | Crystal (39 MHz) | Sleep clock (32.768 kHz) | RAIL's time across deep sleep (sender) |
| --- | --- | --- | --- |
| 440235475 | 0 (reference) | −17.79 to −17.82 (its own reading, 7 runs of one to two minutes); −17.87 (seen from 440240046) | −33.4 (seen from 440240046) |
| 440240046 | −3.73 (seen from 440235475); −3.67 the other way | −9.33 (sender); −9.42 (its own reading, with its crystal) | −25.69 |
| 440235509 | −6.60 | −2.90 | −19.89 |

- **The two ways agree:** a kit's sleep clock seen by the other kit matches its own reading within 0.1 ppm, and the crystals within 0.06 ppm both ways. Within a run (one to two minutes), the rate changes by at most 0.1 ppm between its halves.
- **Between kits,** the crystals differ by up to 6.6 ppm and the sleep clocks by up to 14.9 ppm (440235475 against 440235509; 8.4 and 6.5 ppm for the other pairs). The sleep clocks run 3–18 ppm slow against the crystal of the reference kit. Absolute errors are not known (no reference clock).
- **RAIL's time across deep sleep runs about 16 ppm slower than the sleep clock it is synchronised to** (15.5, 16.4 and 17.0 ppm on the three kits, measured at different times; about the same with frames every 10 ms and every 100 ms, so a rate, not a loss per wake). The cause was not examined. As every node runs the same code, the offset is largely common to sleeping nodes, but a node whose time runs on its crystal (one that does not sleep) sees its children 16 ppm off.
- **Without RAIL's timer sync** (`sl_rail_config_sleep` not called, the default of RAIL's utility components), RAIL's time loses about 0.5 ms at every deep sleep (frames scheduled every 10 ms or 100 ms arrive every 10.498 ms or 100.497 ms): unusable for the protocol. The timing results of §11 and §12.1 are not affected (they are offsets within one wake).

What it means for the stack:
- On these kits two nodes' sleep clocks can differ by 15 ppm, three times the 5 ppm the protocol assumes for its crystals; the drift compensation must take the rate from the parent's beacons, not from a nominal figure. The short-term stability (0.1 ppm over two minutes, about 2 µs of timing noise per frame) is far better than the 1 ppm target, so a rate learnt over one or two 60 s beacon intervals is sufficient while the temperature is steady. **Not measured:** the drift with temperature (a 32.768 kHz tuning-fork crystal typically loses 0.035 ppm/°C² away from 25 °C, about 14 ppm at 5 °C and at 45 °C).
- The firmware must call `sl_rail_config_sleep` with timer sync, and either use the sleep timer directly for its long intervals or account for the 16 ppm of RAIL's time across sleep.

### 12.5 Values for the emulator profile and the stack

| Setting | Value | Source |
| --- | --- | --- |
| Fixed cost of a listening moment (wake, crystal, radio start and stop), recommended settings | 2.43 µC | §12.2 |
| Whole-chip current while the receiver is on (listening, receiving, carrier sense) | 6.03 mA | §12.2 |
| Floor, deep sleep, no debugging in deep sleep | 5.2 µA (kit 440235509) | §12.2 |
| Window opening before the sync word's start | at least 97 µs from the lead alone: the stack's `--rx-lead-us` 100 µs (the clock allowance covers clock error and must not be counted; review, §12.6) | §12.1 |
| Window end after the sync word's end | at least 31–41 µs | §12.1 |
| Answer delay from the frame-end timestamp to the preamble | scheduled delay + 13 µs (assuming the computed preamble time is exact, §12.6); RAIL accepts 220 µs after the timestamp, not 200 µs, counted from the call (at about +58–69 µs here) | §12.3 |
| Carrier sense, start to first bit | its length + 25–32 µs (125 µs for 100 µs) | §12.3 |
| Sleep clocks between nodes / crystals | up to 15 ppm / 6.6 ppm apart (three kits); a sleeping node's RAIL time against an always-on gateway's crystal: 20 to 33 ppm (§12.6) | §12.4 |

Raw logs and energy-monitor dumps of this section are kept in the session's scratch directory only; `run.py` reproduces the measurements (about 10 minutes with the default durations; `--parts` selects).

### 12.6 Review of §12 (2026-11-26)

An independent review checked §12 against the code, the SDK and the raw data. Every figure was reproduced from the dumps and logs, including the charges of §12.2, the edges and the 107 µs shift of §12.1, and the ppm figures of §12.4. The review found the following points, which change what the figures mean.

- **Window opening (§12.1, §12.5).** The specification opens a window at t − a, where the allowance a is the timing uncertainty, so a frame may arrive up to a early. The stack opens at t − a − lead. The receiver therefore has a + lead − e before the sync word, where e is the frame's actual earliness, and this must be at least 97 µs. The lead alone must give the 97 µs, otherwise the allowance loses its meaning. Hence the stack's lead is 100 µs, not 80 µs. The cost is about 0.1 µC per listening moment, about 0.5 µA.
- **Relative rates (§12.4).** The allowance already uses twice the tolerance, so a 15 ppm difference between two kits is 1.5 times the 10 ppm the protocol allows between two nodes, not 3 times. The larger issue: the gateway is always on, so its time runs on its crystal, while a sleeping detector's RAIL time runs on the sleep clock and about 16 ppm slower. A detector can therefore be 20 to 33 ppm off the gateway (for example −19.9 ppm for kit 440235509 against kit 440235475's crystal, 33.4 ppm for kit 440235475 against its own crystal). Emulator runs with ±5 ppm crystals do not represent the kits.
- **Early wake shortened by 12 ticks (§12.2).** Not established as safe:
  - The failure point (−15) was measured in the build without timer sync; in the fast-voltage-scaling build −14 already fails; −15 was not tried in the recommended build.
  - The offset applies to the crystal start-up estimate, read at its 20-tick default on the console build only. The SDK can update that estimate at run time; if it settles near the real 9–10 ticks, −12 leaves 1–2 ticks.
  - A failure stops every window, because the firmware ignores the scheduling status.
  - The 2.43 µC depends on this setting.
- **Floor (§12.2).** 5.2 µA is one kit on one day. This kit's floor has read between 6.1 and 12.3 µA on other days with other firmware, and 10.6 µA in the fast-voltage-scaling build once the radio has run (not explained). This is about 2 µA of uncertainty, 7 % of the 26 µA listening figure.
- **Fit uncertainty (§12.2).** From the four medians: 2.427 µC + 6.036 mA, residual 0.052 µC. Standard errors 0.050 µC and 0.14 mA (not 0.1 mA); with two degrees of freedom, 95 % intervals of about ±0.21 µC and ±0.6 mA. Meter accuracy and differences between kits are not included.
- **Timestamp lag (§12.1, §12.3).** The preamble time is computed from the transmit end timestamp minus RAIL's reported duration, so the data fix only the difference of the two lags (3.5 µs). The receive lag can lie anywhere between 0 and about 23.5 µs. The 97 µs bound does not depend on it. For the turnaround it is better stated as: answer's first bit = received frame's last bit + scheduled delay + 23.5 µs.
- **Turnaround minimum (§12.3).** RAIL refused 200 µs and accepted 220 µs; the 100 µs and 150 µs runs recorded nothing.
- **Windows after a late frame (firmware).** The window counter still skips a window after a frame received past the window's end (7 times in one run). The edge results are not affected.
- **Not covered:** weak signals (all runs at +10 dBm with the kits side by side), other channels, temperature, the recommended build's timing, and the carrier-sense threshold near −75 dBm or a frame starting during the sense.
- **A second review (the analysis code)** recomputed with its own scripts the charges of §12.2 (within 0.01 µC) and the drift figures of §12.4. Every value matches, and no wraparound or sign error was found in the timestamp arithmetic. Points it adds:
  - **Late edge:** the margin after the sync word's end is 31 µs in the default build but 48 µs in the recommended build (one run each). Use about 50 µs.
  - **Phase locking:** each 6 s record samples its 30 wakes at almost the same sampling phase, so the spread between wakes is not a measurement uncertainty. Repeats at other phases agree within 0.013 µC.
  - **Without timer sync:** a listener loses about 45 µs per wake, not 0.5 ms; the 0.5 ms is the sender's loss. Timer sync stays necessary.
  - **Missing charge:** a 0.086 µC event once per second, also without radio, is in neither the floor nor the wake charge. It adds 0.09 µA.
  - **Duration and CRC shift:** RAIL's reported duration after the sync word is 536 µs instead of 533 µs on about 5 % of frames. The run without CRC has its early edge at −108 µs against −116 to −118 µs, so the CRC shift is 99 µs at that edge.
  - **Scripts:** `results.json` uses the opposite sign to §12.4 (noted in `fmlog.py`), and the drift analysis now rejects a run whose frame numbering is inconsistent. `build.sh` reused a generated project after a change of project, PHY or entries; it now generates it again. The turnaround part of `run.py` reads the initiator's log after a fixed 1 s wait.
