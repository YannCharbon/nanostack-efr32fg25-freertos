#!/usr/bin/env python3
"""Sleep floor and wake shapes of an LFN from AEM dumps (commander aem dump CSV: µs, mA, V).

usage: tests/hw/aem_floor.py DUMP.csv [...] [--thr-ma 0.1] [--smooth-us 1000] [--pre-ms 5]
                             [--post-ms 50] [--seg-s 0.1] [--json OUT]

Wakes are runs where the current, averaged over --smooth-us, is above
--thr-ma (runs closer than --merge-ms are merged): the AEM's single samples
are noisy (tens of µA at 10 kHz, more at 100 kHz on the BRD4002A), and in
EM2 the DC-DC converter draws in pulses, so single samples are not
thresholded. The floor is computed on the quiet samples only: every sample
at least --pre-ms before a wake and --post-ms after its end. Its MEAN is
the floor (the samples' distribution is skewed by the pulses: the median
is lower than the charge drawn); reported per file and pooled, with the
spread of the means of --seg-s segments (each segment fully quiet). Per wake: length,
charge above the floor, peak, the median current of its plateaus
(samples within 5-12 mA, the RX plateau; above 20 mA, TX), the median of
its first 200 µs, and the length and charge of its set-up (from the first sample above
0.2 mA to the first RX-plateau sample) and teardown (from the last
RX-plateau sample to the last sample above 0.2 mA). Exit status 1 if
a file has no quiet segment.
"""
import argparse
import csv
import json
import statistics as st
import sys


def load(path):
    t, i = [], []
    with open(path) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            try:
                t.append(int(float(row[0])))
                i.append(float(row[1]))
            except (ValueError, IndexError):
                pass
    return t, i


def pct(xs, p):
    s = sorted(xs)
    return s[min(len(s) - 1, int(p / 100 * len(s)))]


def analyse(path, a):
    t, i = load(path)
    dt = (t[-1] - t[0]) / (len(t) - 1)
    # moving average over smooth_us for the detection only
    w = max(1, int(a.smooth_us / dt))
    sm, acc = [], 0.0
    for k, x in enumerate(i):
        acc += x
        if k >= w:
            acc -= i[k - w]
        sm.append(acc / min(k + 1, w))
    wakes, cur = [], None
    for k, x in enumerate(sm):
        if x >= a.thr_ma:
            if cur and t[k] - t[cur[1]] <= a.merge_ms * 1000:
                cur[1] = k
            else:
                cur = [max(0, k - w + 1), k]
                wakes.append(cur)
    quiet = [True] * len(i)
    pre, post = int(a.pre_ms * 1000 / dt), int(a.post_ms * 1000 / dt)
    for k0, k1 in wakes:
        for k in range(max(0, k0 - pre), min(len(i), k1 + post + 1)):
            quiet[k] = False
    q = [x for x, ok in zip(i, quiet) if ok]
    seg = max(1, int(a.seg_s * 1e6 / dt))
    segs = []
    for s0 in range(0, len(i) - seg + 1, seg):
        if all(quiet[s0:s0 + seg]):
            segs.append(st.mean(i[s0:s0 + seg]) * 1000)
    floor = st.median(q) if q else None
    res = {"file": path, "seconds": round((t[-1] - t[0]) / 1e6, 1), "rate_hz": round(1e6 / dt),
           "mean_uA": round(st.mean(i) * 1000, 2), "wakes": len(wakes),
           "quiet_share": round(len(q) / len(i), 3)}
    if q:
        res.update(floor_median_uA=round(floor * 1000, 2), floor_mean_uA=round(st.mean(q) * 1000, 2),
                   floor_p10_uA=round(pct(q, 10) * 1000, 2), floor_p90_uA=round(pct(q, 90) * 1000, 2))
    if segs:
        res.update(segments=len(segs), seg_min_uA=round(min(segs), 2), seg_max_uA=round(max(segs), 2),
                   seg_sd_uA=round(st.pstdev(segs), 2))
    ws = []
    for k0, k1 in wakes:
        s = i[k0:k1 + 1]
        rx = [x for x in s if 5 <= x <= 12]
        tx = [x for x in s if x > 20]
        # set-up before the first RX-plateau sample and teardown after the
        # last one (wake from EM2, clocks, radio set-up; radio off, EM2 entry)
        krx = [k for k, x in enumerate(s) if 5 <= x <= 12]
        fl = floor or 0
        # raw edges of the wake: first and last sample above 0.2 mA (the
        # smoothed detection widens the wake by up to --smooth-us)
        khi = [k for k, x in enumerate(s) if x > 0.2]
        pre = s[khi[0]:krx[0]] if krx and khi and khi[0] < krx[0] else []
        post = s[krx[-1] + 1:khi[-1] + 1] if krx and khi and khi[-1] > krx[-1] else []
        ws.append({"t_s": round((t[k0] - t[0]) / 1e6, 4), "len_ms": round((k1 - k0 + 1) * dt / 1000, 2),
                   "q_uC": round(sum(x - (floor or 0) for x in s) * dt / 1000, 2), "peak_mA": round(max(s), 2),
                   "rx_ms": round(len(rx) * dt / 1000, 2), "rx_mA": round(st.median(rx), 2) if rx else None,
                   "tx_ms": round(len(tx) * dt / 1000, 2), "tx_mA": round(st.median(tx), 2) if tx else None,
                   "head_mA": round(st.median(s[:max(1, int(200 / dt))]), 2),
                   "pre_ms": round(len(pre) * dt / 1000, 3) if krx else None,
                   "pre_uC": round(sum(x - fl for x in pre) * dt / 1000, 2) if krx else None,
                   "post_ms": round(len(post) * dt / 1000, 3) if krx else None,
                   "post_uC": round(sum(x - fl for x in post) * dt / 1000, 2) if krx else None})
    res["wake_list"] = ws
    return res, q, segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--thr-ma", type=float, default=0.1)
    ap.add_argument("--merge-ms", type=float, default=1.0)
    ap.add_argument("--pre-ms", type=float, default=5.0)
    ap.add_argument("--post-ms", type=float, default=50.0)
    ap.add_argument("--seg-s", type=float, default=0.1)
    ap.add_argument("--smooth-us", type=float, default=1000.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    out, allq, allseg, allw, rc = [], [], [], [], 0
    for p in a.dumps:
        r, q, segs = analyse(p, a)
        out.append(r)
        allq += q
        allseg += segs
        allw += r["wake_list"]
        if not segs:
            rc = 1
        print(f"{p}: {r['seconds']} s at {r['rate_hz']} Hz, mean {r['mean_uA']} uA, {r['wakes']} wakes, "
              f"quiet {r['quiet_share']:.1%}; floor median {r.get('floor_median_uA')} uA, mean "
              f"{r.get('floor_mean_uA')}, p10-p90 {r.get('floor_p10_uA')}-{r.get('floor_p90_uA')}; "
              f"{r.get('segments', 0)} quiet {a.seg_s:g} s segments {r.get('seg_min_uA')}-{r.get('seg_max_uA')} uA")
    pooled = {}
    if allq:
        pooled = {"floor_median_uA": round(st.median(allq) * 1000, 2), "floor_mean_uA": round(st.mean(allq) * 1000, 2),
                  "floor_p10_uA": round(pct(allq, 10) * 1000, 2), "floor_p90_uA": round(pct(allq, 90) * 1000, 2)}
    if allseg:
        pooled.update(segments=len(allseg), seg_min_uA=round(min(allseg), 2), seg_max_uA=round(max(allseg), 2),
                      seg_median_uA=round(st.median(allseg), 2), seg_sd_uA=round(st.pstdev(allseg), 2))
    print("pooled:", json.dumps(pooled))
    if allw:
        def summ(key, ws):
            v = [w[key] for w in ws if w[key] is not None]
            return f"median {st.median(v)} min {min(v)} max {max(v)}" if v else "none"
        short = [w for w in allw if not w["tx_ms"]]
        print(f"wakes: {len(allw)}; without TX: {len(short)}")
        for key in ("len_ms", "q_uC", "peak_mA", "rx_ms", "rx_mA", "head_mA", "pre_ms", "pre_uC", "post_ms", "post_uC"):
            print(f"  {key}: {summ(key, short)}")
        txw = [w for w in allw if w["tx_ms"]]
        if txw:
            print(f"  with TX ({len(txw)}): tx_mA {summ('tx_mA', txw)}, tx_ms {summ('tx_ms', txw)}, "
                  f"q_uC {summ('q_uC', txw)}")
    if a.json:
        json.dump({"files": out, "pooled": pooled}, open(a.json, "w"), indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
