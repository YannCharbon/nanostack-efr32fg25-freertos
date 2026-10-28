#!/usr/bin/env python3
"""S2: classify the LFN's wakes over many AEM dumps (aem_*.csv, AEM µs clock is continuous).

usage: s2_analyse.py DIR [--uc-ms 5000] [--bc-ms 10000] [--thr-ma 1.0] [--json OUT]

Per wake: start, length, charge above the floor, peak, and the length of the RX plateau
(the longest run of samples between --rx-lo and --rx-hi mA). Wakes are put in series by
phase: unicast listen (period uc), LFN broadcast slot (period bc, the remaining ones), other.
In each series, a wake much longer than the median is reported separately (LTS reception,
frame received). Window widening: RX plateau length against time since the last long BC wake.
"""
import argparse
import csv
import glob
import json
import os
import statistics as st


def wakes(path, thr, merge_us, rx_lo, rx_hi):
    t, i = [], []
    with open(path) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            t.append(int(row[0]))
            i.append(float(row[1]))
    if len(t) < 2:
        return [], None
    dt = (t[-1] - t[0]) / (len(t) - 1)
    low = [x for x in i if x < thr]
    floor = st.median(low) if low else 0.0
    out, cur = [], None
    for k, x in enumerate(i):
        if x >= thr:
            if cur and t[k] - cur["end"] <= merge_us:
                cur["end"] = t[k]
                cur["k1"] = k
            else:
                cur = {"start": t[k], "end": t[k], "k0": k, "k1": k}
                out.append(cur)
    for w in out:
        seg = i[w["k0"]:w["k1"] + 1]
        w["len_ms"] = (w["end"] - w["start"] + dt) / 1000
        w["q_uC"] = sum(x - floor for x in seg) * dt / 1000
        w["peak"] = max(seg)
        best = run = 0
        for x in seg:
            run = run + 1 if rx_lo <= x <= rx_hi else 0
            best = max(best, run)
        w["rx_ms"] = best * dt / 1000
        del w["k0"], w["k1"]
    info = {"file": os.path.basename(path), "t0": t[0], "t1": t[-1], "mean_uA": st.mean(i) * 1000,
            "floor_uA": floor * 1000}
    return out, info


def phase_series(ws, period_us, tol_us):
    """Return the indices of wakes on the dominant phase of the given period."""
    if not ws:
        return set()
    bins = {}
    for k, w in enumerate(ws):
        b = int((w["start"] % period_us) // tol_us)
        bins.setdefault(b, []).append(k)
    best = max(bins.values(), key=len)
    ref = st.median([ws[k]["start"] % period_us for k in best])
    sel = set()
    for k, w in enumerate(ws):
        d = (w["start"] - ref) % period_us
        if min(d, period_us - d) <= 2 * tol_us:
            sel.add(k)
    return sel


def describe(name, ws, T):
    if not ws:
        print(f"  {name}: none")
        return {}
    lens = [w["len_ms"] for w in ws]
    q = [w["q_uC"] for w in ws]
    rx = [w["rx_ms"] for w in ws]
    gaps = [(b["start"] - a["start"]) / 1e6 for a, b in zip(ws, ws[1:])]
    d = {"n": len(ws), "per_s": len(ws) / T, "len_med": st.median(lens), "len_min": min(lens), "len_max": max(lens),
         "rx_med": st.median(rx), "rx_min": min(rx), "rx_max": max(rx), "q_med": st.median(q), "q_mean": st.mean(q),
         "uA": sum(q) / T, "gap_med": st.median(gaps) if gaps else None}
    print(f"  {name:22s} n {d['n']:5d} ({d['per_s']:.3f}/s) gap {d['gap_med'] if gaps else 0:8.3f} s  len {d['len_med']:6.2f} "
          f"[{d['len_min']:.2f}-{d['len_max']:.2f}] ms  rx {d['rx_med']:5.2f} [{d['rx_min']:.2f}-{d['rx_max']:.2f}] ms  "
          f"q {d['q_med']:6.1f} uC  -> {d['uA']:6.2f} uA")
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--uc-ms", type=float, default=5000)
    ap.add_argument("--bc-ms", type=float, default=10000)
    ap.add_argument("--thr-ma", type=float, default=1.0)
    ap.add_argument("--rx-lo", type=float, default=8.5)
    ap.add_argument("--rx-hi", type=float, default=12.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, "aem_*.csv")))
    allw, infos = [], []
    for p in files:
        w, info = wakes(p, a.thr_ma, 300, a.rx_lo, a.rx_hi)
        if info:
            allw += w
            infos.append(info)
    T = sum((x["t1"] - x["t0"]) / 1e6 for x in infos)
    mean = sum(x["mean_uA"] * (x["t1"] - x["t0"]) for x in infos) / sum(x["t1"] - x["t0"] for x in infos)
    print(f"{len(files)} files, {T:.0f} s recorded, mean {mean:.2f} uA, floor median "
          f"{st.median([x['floor_uA'] for x in infos]):.2f} uA, wakes {len(allw)}")
    print("  per-file mean uA: " + " ".join(f"{x['mean_uA']:.1f}" for x in infos))
    allw.sort(key=lambda w: w["start"])
    uc = phase_series(allw, a.uc_ms * 1000, 20000)
    rest = [w for k, w in enumerate(allw) if k not in uc]
    bc_idx = phase_series(rest, a.bc_ms * 1000, 20000)
    ucw = [allw[k] for k in sorted(uc)]
    bcw = [rest[k] for k in sorted(bc_idx)]
    other = [w for k, w in enumerate(rest) if k not in bc_idx]
    res = {"T_s": T, "mean_uA": mean, "files": infos}

    def split(ws):
        if not ws:
            return ws, []
        m = st.median(w["len_ms"] for w in ws)
        return [w for w in ws if w["len_ms"] <= 1.4 * m], [w for w in ws if w["len_ms"] > 1.4 * m]
    uc_n, uc_l = split(ucw)
    bc_n, bc_l = split(bcw)
    res["uc"] = describe("unicast listen", uc_n, T)
    res["uc_long"] = describe("unicast listen, long", uc_l, T)
    res["bc"] = describe("LFN broadcast slot", bc_n, T)
    res["bc_long"] = describe("broadcast slot, long", bc_l, T)
    res["other"] = describe("other wakes", other, T)
    res["floor_uA"] = st.median([x["floor_uA"] for x in infos])
    # widening: RX plateau vs time since the last long BC wake (LTS)
    lts = [w["start"] for w in bc_l]
    pts = []
    for w in uc_n + bc_n:
        prev = [x for x in lts if x <= w["start"]]
        if prev:
            pts.append(((w["start"] - prev[-1]) / 1e6, w["rx_ms"], w["len_ms"]))
    if pts:
        print("  window vs time since last long broadcast-slot wake (LTS):")
        for lo in range(0, 400, 10):
            sel = [p for p in pts if lo <= p[0] < lo + 10]
            if sel:
                print(f"    {lo:3d}-{lo + 10:3d} s: n {len(sel):4d}  rx {st.median(p[1] for p in sel):5.2f} ms  "
                      f"wake {st.median(p[2] for p in sel):5.2f} ms")
        res["widening"] = pts
    if len(lts) > 1:
        g = [(b - a) / 1e6 for a, b in zip(lts, lts[1:])]
        print(f"  long BC wakes (LTS) interval: median {st.median(g):.3f} s, min {min(g):.3f}, max {max(g):.3f}")
    if len(ucw) > 2:
        g = [(y["start"] - x["start"]) / 1e6 for x, y in zip(ucw, ucw[1:]) if (y["start"] - x["start"]) < 1.5 * a.uc_ms * 1000]
        print(f"  unicast period: median {st.median(g):.5f} s (n {len(g)})")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
