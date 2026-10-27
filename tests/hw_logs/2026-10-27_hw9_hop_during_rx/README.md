# HW9: frames cut by a channel change during RX (2026-10-27)

Two BRD4271A kits (EU), stack `2846ee0`. One boot: reset the BR, 3 s later the
RN, then wait up to 420 s for `ARM_NWK_BOOTSTRAP_READY`. Variants were run in
interleaved blocks of 3 boots:

- A: TX watchdog + TRNG pre-fill, `RAIL_PHY_HOP_DEFER=0` (the firmware before the fix);
- B: as A without the TRNG pre-fill;
- C: as A with `RAIL_PHY_HOP_DEFER=1` (the fix).

Results: A 1 stall in 15 boots, B 1 in 15, C 0 in 30. All three builds traced
"hop X->Y during RX"; A and B lost hundreds of frames that way, C none (each
change was deferred to the end of the frame).

- `summary.txt`: one line per boot (`analyze.py` output), then per-variant totals.
- `A-03_rn_hops_excerpt.txt`: the stalled boot A-03 on the RN. The BR's
  614-byte EAP-TLS frame is cut at every retry by the RN's unicast hops.
- `campaign.sh`, `analyze.py`: the scripts as run (paths point to the scratch
  directory the campaign used).
