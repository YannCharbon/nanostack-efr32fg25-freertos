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
- The kits are left with the RAILtest mode-3 image on all four
  (440240046, 440235509, 440290182, 440235475); reflash the firmware before a
  firmware run.

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
