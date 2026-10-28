#!/usr/bin/env python3
"""Find wake-ups in an AEM dump (commander aem dump CSV: us, mA, V).

usage: aem_events.py DUMP.csv [--thr-ma 1.0] [--merge-us 300] [--list N]

An event is a run of samples above the threshold; runs closer than --merge-us
are merged. Prints the floor (median of samples below the threshold), the
average current, the events' count, interval, length and charge, and a
histogram of lengths and of charge; with --list N, the first N events.
"""
import argparse
import csv
import statistics as st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--thr-ma", type=float, default=1.0)
    ap.add_argument("--merge-us", type=float, default=300)
    ap.add_argument("--list", type=int, default=0)
    a = ap.parse_args()
    t, i = [], []
    with open(a.dump) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            t.append(int(row[0]))
            i.append(float(row[1]))
    dt = (t[-1] - t[0]) / (len(t) - 1)
    T = (t[-1] - t[0]) / 1e6
    floor = st.median([x for x in i if x < a.thr_ma] or [0])
    ev = []
    cur = None
    for k, x in enumerate(i):
        if x >= a.thr_ma:
            if cur and t[k] - cur[1] <= a.merge_us:
                cur[1] = t[k]
                cur[2] += (x - floor) * dt
                cur[3] = max(cur[3], x)
            else:
                cur = [t[k], t[k], (x - floor) * dt, x]
                ev.append(cur)
    print(f"{a.dump}: {len(i)} samples, {T:.1f} s, {1e6 / dt:.0f} Hz")
    print(f"  average {st.mean(i) * 1000:.1f} uA, floor (median < {a.thr_ma} mA) {floor * 1000:.2f} uA")
    if not ev:
        return
    lens = [(e[1] - e[0] + dt) / 1000 for e in ev]          # ms
    q = [e[2] / 1000 for e in ev]                          # uC above floor (mA*us/1000)
    gaps = [(ev[k + 1][0] - ev[k][0]) / 1e6 for k in range(len(ev) - 1)]
    print(f"  events {len(ev)}  ({len(ev) / T:.3f}/s)")
    print(f"  length ms: median {st.median(lens):.2f} min {min(lens):.2f} max {max(lens):.2f}")
    print(f"  charge uC above floor: median {st.median(q):.2f} mean {st.mean(q):.2f} max {max(q):.1f}; "
          f"sum {sum(q):.0f} uC = {sum(q) / T:.1f} uA")
    if gaps:
        print(f"  interval s: median {st.median(gaps):.3f} min {min(gaps):.3f} max {max(gaps):.3f}")
    peak = [e[3] for e in ev]
    print(f"  peak mA: median {st.median(peak):.1f} max {max(peak):.1f}")
    edges = [0.5, 1, 2, 5, 10, 20, 50, 100, 1e9]
    h = [sum(1 for x in lens if lo <= x < hi) for lo, hi in zip([0] + edges[:-1], edges)]
    print("  length histogram (ms): " + " ".join(f"<{e}:{n}" for e, n in zip(edges, h) if n))
    for e, L, c in list(zip(ev, lens, q))[:a.list]:
        print(f"    t={(e[0] - t[0]) / 1e6:9.4f} s len {L:7.2f} ms  q {c:7.2f} uC  peak {e[3]:6.2f} mA")


if __name__ == "__main__":
    main()
