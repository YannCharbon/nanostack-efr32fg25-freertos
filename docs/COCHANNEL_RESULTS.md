# Two overlapping FSK frames on the FG25: measurements for the emulator's medium

Measured on 2026-11-09 with three EFR32FG25 kits (BRD4271A, 868 MHz) running
Silicon Labs' RAILtest. Purpose: calibrate the emulator's central radio medium
(EM4/EM5, emulator `docs/MEDIUM.md`). The medium currently treats interference
as noise in its empirical loss curve, which needs about 20 dB of SINR for 1 %
loss on a 50-byte frame, and it lets a frame at least 6 dB stronger take over
during the whole SHR of the frame being received.

Scripts: `tests/hw/cochannel/`. Raw results (one JSON line per cycle or per
point, and every console line): `/home/yann/kit-runs/2026-11-09_cochannel/`.

## 1. Summary

| Question | Measured (EU mode 3, 150 kbit/s) | Emulator today |
| --- | --- | --- |
| Frame already locked, interferer over its PHR and payload, 50-byte frame | 50 % received at SIR 2.7 dB, 90 % at 3.8 dB, 99 % at 5.0 dB | 50 % at 11.5 dB, 90 % at 15.3 dB, 99 % at 20.0 dB |
| Same, 250-byte frame | 50 % at 3.9 dB, 90 % at 5.3 dB, 99 % at 6.8 dB | 50 % at 14.6 dB, 99 % at 23.0 dB |
| Same, interferer over the last 0.83 ms only (50 bytes) | 50 % at 1.1 dB, 90 % at 2.1 dB, 99 % at 3.1 dB | follows the overlapped bits |
| Two frames at about equal power (within about 1 dB) | both lost | both lost |
| Capture by a stronger later frame | only if it starts in the first 200 µs of the first frame's preamble, or 650–750 µs after its start (just before the first frame's sync word ends); from 300 to 550 µs both frames are lost; never after the sync word | whole SHR (0–747 µs), 6 dB margin |
| Capture margin | about 3 dB (90–100 % received from 2.5–4 dB) | 6 dB |
| Modulated interferer on the adjacent channel (±200 kHz) | about 10 dB weaker than the same interferer co-channel: 50 % lost at W/I ≈ −9 dB, none lost from −5 dB | not modelled (no effect) |
| Modulated interferer two channels away, or a CW tone on the adjacent channel | no loss up to the largest ratio we could make (−29.8 dB) | not modelled (no effect) |
| EU mode 1a (50 kbit/s), 50-byte locked frame | 50 % at 2.9 dB, 90 % at 4.0 dB, 99 % at 5.3 dB | as mode 3 |

The collision losses of the current medium are therefore strongly pessimistic
for a locked frame (by 9–16 dB of SIR), and its capture rule is both too lax in
timing (the real radio cannot switch during most of the preamble) and too
strict in margin (3 dB is enough). Parameters for the medium are in §6.

## 2. Setup

- **Kits:** R (receiver) 440240046, W ("wanted") 440235509, I (interferer)
  440290182, all BRD4271A on BRD4001A. The fourth kit, 440235475, is a
  BRD4270B (902–928 MHz matching); it was used only as a stronger continuous
  interferer (§5), where its level at R is measured like any other. All four
  run RAILtest 2.19.3 (Simplicity SDK 2025.6, our `.deps/simplicity_sdk`),
  built by `tests/hw/cochannel/build_railtest.sh`.
- **PHYs:** the firmware's own radio configurations:
  `PHY_WISUN_FAN_1v1_863MHz_Plan33_2FSK_3_EU` (mode 3: 150 kbit/s, h = 0.5,
  200 kHz channels, 12-byte preamble, 2-byte SFD, so the SHR lasts 747 µs) and
  `PHY_WISUN_FAN_1v1_863MHz_Plan32_2FSK_1a_EU` (mode 1a: 50 kbit/s, 100 kHz
  channels, 8-byte preamble, SHR 1600 µs). Demodulator TRECS Viterbi; the
  Radio Configurator reports a sync-word timeout of 800 µs for mode 3. SUN FSK
  PHR, 4-byte FCS, whitening on. RX options as in our firmware (no collision
  detection, no address filtering).
- **One change to RAILtest:** received packets are printed with only the
  first 4 payload bytes (a copy of `app_main.c` in the build directory; the
  SDK is not modified), so the 115200-baud console keeps up.
- **Levels:** the kits stand a few tens of centimetres apart; TX power
  −15 … +15 dBm gives −38 … −14 dBm at R, far above sensitivity (about
  −105 dBm for mode 3) and far below saturation (+10 dBm). The wanted level
  is therefore 70 dB above sensitivity: noise plays no part, unlike the
  datasheet's tests (wanted signal 3 dB above sensitivity). The FG25's
  power table is coarse and not monotonic below −9 dBm, so every point uses
  the levels measured at R, never the TX setting.
- **SIR** of a frame = its mean packet RSSI at R when sent alone minus the
  other frame's mean RSSI when sent alone, both from the same run (one cycle
  in ten sends W alone and one sends I alone). RSSI is reported in whole dB;
  the mean of 10–40 frames gives a resolution of a few tenths of a dB. Points
  with the same nominal SIR but different power pairs differ by up to about
  1 dB in their transition, which we take as the accuracy of the SIR axis.
- **Other traffic:** the lab's Wi-SUN network (MTS bench) hops over the same
  channels and reaches R at −63 … −85 dBm, about 13 frames per 30 s on one
  channel. It is 25 dB or more below our frames; a cycle in which W or I
  caught one of its frames instead of the trigger is discarded (below).

## 3. Method

### 3.1 Two frames with a set time offset (`cochannel.py`, `sweep.py`)

Each cycle, R sends a short trigger frame (10 bytes) and returns to RX. W and
I wait in RX with RAILtest's `txAfterRx d`: each sends one frame `d` µs after
the trigger's sync-word end, from the idle state (so a frame on air cannot
postpone it). I always sends first (`d_I` = 3000 µs, 5000 µs in mode 1a); W
sends `delta` µs later. The offsets are hardware-timed: the sync-word end of
W's frame at R falls within ±5 µs of its nominal time (calibration runs:
`d` + 443 µs after the trigger's end for mode 3, `d` + 677 µs for mode 1a).

R reports every frame it synchronises on, CRC failures included
(`IGNORE_CRC_ERRORS | TRACK_ABORTED`). A frame counts as received when R
reports it at its expected time (±100 µs) with a good CRC and the right
payload marker (W 0xAA, I 0xBB). The host checks on W's and I's consoles
that each one received the trigger and completed its TX; cycles that fail
this check are discarded (9.1 % of all cycles, independent of the outcome at
R since the check concerns what happened before the two frames).

In all, 70 450 cycles, 64 028 valid, of which 50 715 with both frames.

### 3.2 Continuous interferer (`stream.py`)

W sends 200 frames of 50 bytes, 5 ms apart; R counts them with RAILtest's
counters. The interferer sends without pause either a CW tone (`setTxTone`,
the datasheet's method) or a PN9 stream with the PHY's own modulation
(`setTxStream 1`), on R's channel or 1 or 2 channels away. W's level is its
packet RSSI; the interferer's is R's RSSI reading (`getRssi`) on the
interferer's channel. Check of the two methods on one transmitter
(`mode3/rssi_method_check.txt`): `getRssi` reads 0.8–1.2 dB above the packet
RSSI for PN9 and 1.0–1.8 dB above for a tone, so the W/I ratios of §5 are
about 1 dB lower than a packet-to-packet SIR.

## 4. Results, EU mode 3

### 4.1 Locked frame with an interferer over the rest of it

I's frame (the first) has its sync word before W's frame starts
(`delta` = 900 µs: W begins 153 µs after I's sync-word end, so W covers I's
PHR, payload and FCS). Outcome of the first frame vs its SIR (logistic fit
by maximum likelihood, `fit.py`):

| Frame | Overlap | Points / cycles | 50 % received | 90 % | 99 % |
| --- | --- | --- | --- | --- | --- |
| 50 bytes | PHR to FCS (2727 µs) | 31 / 6306 | +2.65 dB | +3.8 dB | +5.0 dB |
| 250 bytes | PHR to FCS (13 393 µs) | 42 / 6434 | +3.9 dB | +5.3 dB | +6.8 dB |
| 50 bytes, `delta` = 2800 µs | last 827 µs | 31 / 4407 | +1.1 dB | +2.1 dB | +3.1 dB |

Selected points, 50 bytes (`fine_l50_d900`): SIR +4.5 dB 200/206 received,
+3.8 dB 179/200, +2.7 dB 114/204, +2.0 dB 20/205 and 61/201, +1.2 dB 24/206,
+0.4 dB and below 0 of about 200 per point. In the coarse sweep
(`cap_l50_d900`) the first frame is received in 81 of 81 to 86 of 86 cycles
at every SIR from +5.5 to +22.6 dB.

The second frame of these cycles (W) is never received, even 22 dB stronger
(0 of 85): once the radio holds a sync word it does not let go. At equal
power both frames are lost (e.g. 0 of 204 and 0 of 205 for either frame).

The transition is steep (logistic scale 0.5–0.6 dB). The emulator's form,
`BER = min(0.5, 0.5·exp(−(SIR_dB − x0)/b))` over the overlapped bits, fits
the three cases together with **x0 = −2.85 dB, b = 0.90 dB**
(`fit.py --ber 409:… 2009:… 124:…`): it gives 50 % at 2.3 / 3.7 / 1.2 dB and
99 % at 6.1 / 7.5 / 5.0 dB for the three overlaps, within 0.4 dB of the
logistic fits at 50 % and 1–2 dB conservative at 99 %. The emulator today
uses x0 = 0, b = 2 (50 % at 11.5 dB and 99 % at 20 dB for 432 counted bits).

### 4.2 Capture: which frame wins when the second starts during the first's SHR

The second frame (W) stronger by +5, +11 or +21 dB (pairs −5.1/−15,
+3.0/−15, +15/−15 dBm; 60–156 cycles per cell; `mode3_dscan/`). Received
frames of W (I's frame is lost in every cell except where noted):

| W starts after I (µs) | 0 | 100 | 150 | 200 | 250 | 350–500 | 550 | 600 | 650 | 700 | 750 | 800–2000 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| +5 dB | 65/65 | 64/64 | 61/61 | 64/64 | 81/154 | 0 of 60–65 each | 0/60 | 68/156 | 146/150 | 149/153 | 71/152 | 0 of 61–66 each |
| +11 dB | 60/60 | 66/66 | 65/65 | 60/61 | 103/155 | 0 of 61–66 each | 6/151 | 37/150 | 63/64 | 63/63 | 98/154 | 0 of 61–66 each |
| +21 dB | 61/61 | 65/65 | 64/64 | 65/65 | 96/156 | 0 of 61–66 each | 0/61 | 20/154 | 59/60 | 62/63 | 79/152 | 0 (1 of 65 at 2000) |

So, for the frame that starts second and is clearly stronger:

- **0–200 µs** (first 30 of the 96 preamble bits): it is received.
- **300–550 µs**: neither frame is received. R reports no sync at all: the
  radio has started on the first preamble, the stronger frame destroys the
  first sync word, and the search ends before the second sync word arrives.
- **650–700 µs** (the second frame starts while the first frame's SFD is on
  air, 640–747 µs): it is received again, as the radio restarts its search
  after the first sync word is missed and still finds the second preamble.
- **from 800 µs** (after the first sync word): never; the first frame is
  then lost too unless it is clearly stronger (§4.1).
- 250, 600 and 750 µs are transitions (13–97 %).

Margin, from full SIR sweeps at fixed offsets (cycles per point 80–307;
`cap_l50_d0`, `cap_l50_d100`, `cap_l50_d700`, `cap_l250_d100`):

| Offset | Stronger frame received |
| --- | --- |
| 0 µs (same start), 50 bytes | +0.3 dB 240/305, +2.2 dB 284/300, +4.8 dB and above 100 % (80–87 per point) |
| 100 µs, 50 bytes | +0.9 dB 173/300, +2.7 dB 271/303, +4.4 dB 83/84, +5.1 dB and above 100 % |
| 100 µs, 250 bytes | +1.1 dB 88/203, +3.7 dB 196/204, +4.7 dB and above 100 % (but 62/63 at +11.7 dB) |
| 700 µs, 50 bytes | +1.7 dB 222/307, +3.8 dB 85/87, +4.6 dB 299/305, +6.3 dB 294/300, above that 98–100 % |

When the first frame is the stronger one, it is received from about +2.4 dB
whatever the offset (e.g. offset 300 µs: 86/86 at +2.4 dB, 84/85 at +3.5 dB,
100 % above). The order of arrival hardly matters inside the 0–200 µs window: the
stronger frame wins with about 3 dB of margin.

### 4.3 Continuous co-channel interferer

`co_pn9`, `co_tone` (200 frames per point; W/I as measured, see §3.2 for the
≈1 dB offset of the W/I axis):

| W/I | −2.0 | −0.9 | +0.3 / +0.4 | +1.4 | +2.1 and above |
| --- | --- | --- | --- | --- | --- |
| PN9 (modulated) | 0/200 | 49/200 | 172/200, 137/200 | 195/200 | 199–200/200 |

| W/I | −2.9 | −2.0 | −0.5 | +0.4 and above |
| --- | --- | --- | --- | --- |
| CW tone | 1/200 | 160/200 | 196/200 | 195–200/200 |

A continuous modulated interferer needs about W/I ≥ +2 dB (about +3 dB in
packet-RSSI terms) for no loss, like a framed interferer over the whole
frame. A CW tone at the channel centre is about 1.5–2 dB less harmful. The
datasheet's co-channel rejection (CW interferer, wanted 3 dB above
sensitivity) is −4.4 dB for mode 3 with FEC and −7.1 dB for mode 1a without
FEC, i.e. the wanted signal must be 4.4–7.1 dB above the tone; with the
wanted signal far above sensitivity, 98 % are received at −0.5 dB. The datasheet
figure includes the noise at sensitivity and is the conservative one.

### 4.4 Adjacent and alternate channels

Interferer at ±200 kHz (`adj_*`) and ±400 kHz (`alt_*`), W at −36 dBm at R,
200 frames per point; the strongest points use the fourth kit (−7 dBm at R).

| Interferer | W/I where frames start to be lost | Points |
| --- | --- | --- |
| PN9, +1 channel | −9.5 dB: 151/200; −11.6 dB: 18/200; −13 dB and below: 0 | −6.2 dB: 198/200; −5.2 dB and above: 200/200 |
| PN9, −1 channel | −9.2 dB: 74/200; −11.0 dB: 14/200; −12.3 dB and below: 0 | −6.8 dB: 189/200; −5.0 dB and above: 200/200 |
| PN9, ±2 channels | none | 198–201/200 at every point down to −29.8 dB |
| CW tone, ±1 channel | none | 198–201/200 at every point down to −29.5 dB |

A modulated signal on the next channel acts like a co-channel interferer
about 10 dB weaker (50 % lost at W/I ≈ −9.5 dB against ≈ −0.3 dB co-channel,
both on the same W/I axis). This is the transmitter's own spectrum: 150 kbit/s
2-FSK with h = 0.5 fills more than the 200 kHz channel (Carson bandwidth
225 kHz), so part of the interferer's power falls in our channel; the
receiver's selectivity against an unmodulated carrier is much better (the
datasheet gives 45.5 dB with a CW, we could only show more than 29.5 dB).
Two channels away, no effect down to −29.8 dB (datasheet, CW: 47.2 dB).

### 4.5 RX-to-TX turnaround (`turnaround.py`)

RAILtest's default state timings on this PHY: idle→RX 120 µs, idle→TX
120 µs, RX→TX 130 µs, TX→RX 100 µs, TX→TX 129 µs (our firmware sets RX→TX
192 µs, TX→RX 182 µs, idle→TX and idle→RX 100 µs; `drivers/rail/rail_phy.c`).
Measured with W answering the trigger from the idle state (20 cycles per
step): a TX scheduled less than 25 µs after the end of the received frame
fails (0/20 at 3 µs and at 28 µs, 11/20 at 53 µs, 18–20/20 from 78 µs). The
answer's preamble starts at R 494–500 µs after the trigger's end when it is
scheduled 53 µs after that end; the delay from the scheduled time to the first preamble bit at R
is therefore about 440 µs (this includes R's own demodulation delay, a few
tens of µs, which we could not separate). This is RAIL's scheduled-TX path
from idle, not our MAC's ACK path; the MAC's real ACK turnaround still has to
be measured with the firmware (EM5).

## 5. Results, EU mode 1a

Same method (`mode1a/`), 50-byte frames (10 240 µs on air, SHR 1600 µs).

- **Locked frame, interferer from 1800 µs to the end** (38 points, 5718
  cycles): 50 % received at +2.9 dB, 90 % at +4.0 dB, 99 % at +5.3 dB; e.g.
  +3.1 dB 143/206, +4.4 dB 189/202, +5.2 dB 120/123, +7.2 dB and above 100 %.
  Within 0.3 dB of mode 3. Fitted form: x0 = −2.40 dB, b = 0.90 dB.
- **Capture** (second frame +5.5 dB / +11 dB stronger):

| W starts after I (µs) | 0 | 100 | 200 | 400 | 600 | 800–1700 |
| --- | --- | --- | --- | --- | --- | --- |
| +5.5 dB | 61/61 | 100/153 | 103/155 | 32/153 | 11/155 | 0 of 61–65 each |
| +11 dB | 65/65 | 66/66 | 66/66 | 66/66 | 146/156 | 0 (1 of 66 at 1500) |

  The early window is longer in time (about 400–600 µs, 20–30 bits, more
  with a larger margin), and no late window appears around the SFD
  (1280–1600 µs). Same start (`cap_l50_d0`): the stronger frame wins from
  +1.3 dB (67/67; +0.1 dB 161/206); equal power, both lost (0 of 67).

## 6. Parameters for the emulator's medium

1. **Interference loss curve.** Keep today's curve for noise, and use for the
   bits overlapped by an interferer the same form with measured constants:
   `BER_int = min(0.5, 0.5·exp(−(SIR_dB + 2.85) / 0.90))` (mode 3; mode 1a
   gives +2.40 instead of +2.85, take 2.85 for every FSK mode), with SIR the
   frame's power over the sum of the interferers' powers on that segment.
   Where noise and interference both matter, take the larger of
   `BER_noise(SNR)` and `BER_int(SIR)` per segment. Expected effect: a 50-byte
   frame survives an overlapping frame 6 dB weaker in 99 % of cases (today
   about 20 dB is needed). Not measured near sensitivity, where the noise and
   the interference add (the datasheet's −4.4 dB/−7.1 dB CW figures are that
   case).
2. **Capture margin: 3 dB** (`--medium-capture-db=3`).
3. **Capture timing** (replace "the whole SHR"). For a frame B starting `t`
   after the frame A being received, with A's preamble `Tp` and SHR `Tshr`:
   - `t < 0.3·Tp` (mode 3: 200 µs of 640; mode 1a: about 400 µs of 1280):
     the stronger by ≥ 3 dB is locked, whichever came first; within 3 dB,
     both are lost;
   - mode 3 only, `Tp ≤ t < Tshr` (during A's SFD): B is locked if ≥ 3 dB
     stronger (about 98 % measured at 650–700 µs);
   - otherwise, if B is ≥ 2.5 dB stronger (enough to break A's sync word),
     and `t < Tshr`: both frames are lost (R syncs on neither);
   - after A's SHR: no capture; B is interference to A (item 1).
   The windows are set in bits by the demodulator's preamble detection and
   its 800 µs sync timeout (mode 3), so they are PHY-specific: use the
   mode-3 values for mode 3 and the mode-1a values for 1a, and treat other
   PHYs as unknown.
4. **Preamble length.** The Silicon Labs mode-3 PHY sends a 12-byte preamble
   (747 µs SHR); the emulator's default is 8 bytes (`WSSIM_PHY_PREAMBLE`).
   Set 12 bytes when the medium models the FG25 in mode 3 (8 bytes is right
   for mode 1a).
5. **Adjacent channels.** A frame on the next channel interferes as if it were
   co-channel and **10 dB** weaker (adjacent rejection for an FSK interferer of
   the same PHY); two channels away or more, ignore it (more than 30 dB,
   datasheet 47 dB with a CW).
6. **Turnaround.** From a scheduled TX in idle, the preamble goes on air
   about 0.44 ms after the scheduled time, and a TX cannot be scheduled
   reliably earlier than about 80 µs after the end of the received frame. This is a
   lower bound for the medium's RX-to-TX timing, not our MAC's ACK delay.

## 7. Limits

- One wanted level only (−38 … −14 dBm at R); nothing near sensitivity.
- Three positions on a desk, no fading; the SIR axis is accurate to about
  1 dB (RSSI in whole dB, power table steps, differences between kits).
- The FG25's TX power does not go below −15 dBm, so the strongest interferer
  ratio we could make is −23 dB with our kits and −30 dB with the fourth kit;
  CW adjacent and alternate-channel rejection are only shown to exceed that.
- Capture timing was scanned at three margins in mode 3 and two in mode 1a;
  the transition points (250, 600, 750 µs in mode 3) are coarse.
- Frames of 50 and 250 bytes only in mode 3, 50 bytes in mode 1a; no mode 1a
  adjacent-channel test.
- RAILtest only: our firmware's MAC (CCA, its own ACK timing) was not in the
  loop.
- The kits are left with RAILtest (mode 3 image); reflash the firmware before
  the next firmware run.

## 8. Reproducing

```bash
tests/hw/cochannel/build_railtest.sh mode3 <build dir>     # and mode1a
tests/hw/cochannel/flash.sh <build dir>/mode3/build/debug/rail_soc_railtest.s37 440240046 440235509 440290182
tests/hw/cochannel/queue_mode3.sh <out>/mode3              # §4.1, §4.2 sweeps (about 1 h)
tests/hw/cochannel/queue_mode3b.sh <out>/mode3             # 250 bytes high SIR, §4.3, §4.4 (about 25 min)
tests/hw/cochannel/queue_mode3c.sh <out>/mode3_dscan       # capture timing grid (about 10 min)
tests/hw/cochannel/queue_mode3d.sh <out>/mode3             # alternate channel (about 6 min)
tests/hw/cochannel/turnaround.py <out>/turnaround
# mode 1a: flash the mode1a image, then
tests/hw/cochannel/queue_mode1a.sh <out>/mode1a 677        # 677 = calibrated offset (us)
tests/hw/cochannel/analyze.py <out>/mode3 'fine_*'         # tables
tests/hw/cochannel/fit.py <out>/mode3/fine_l50_d900.jsonl  # logistic fit
tests/hw/cochannel/analyze_stream.py <out>/mode3 'adj_*'
```

The coarse 0/100/300/…/1200 µs scan of §4.2 was run as
`SWEEP_ARGS='--pairs 150:-150,30:-150,-51:-150 --n 60 --n-near 150' run_capture.sh <out>/mode3_dscan 50 0 100 200 400 500 600 700 800 1200`.
