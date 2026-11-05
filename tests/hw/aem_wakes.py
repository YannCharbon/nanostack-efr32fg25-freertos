#!/usr/bin/env python3
"""Classify an LFN's wakes in an AEM dump analysed by aem_floor.py (--json).

usage: tests/hw/aem_wakes.py DUMP.json [...] [--line]

Classes (the decomposition of the firmware README, "LFN current against the
power formula" and "Housekeeping wakes removed", 2026-11-05):
- window:     radio on (RX plateau) for more than 2 ms: listening windows,
              with or without traffic; with TX: an uplink or an ACK sent;
- sync proc:  no radio, >= 1 uC, starting within 60 ms after a window ends:
              the processing of a received sync frame (4.2-4.4 uC, about 24 ms
              after its window);
- timer:      no radio, >= 1 uC, otherwise: the stack's timer wakes of about
              3 ms and 6 uC (before stack aa2a828: the housekeeping every 20 s
              and more, 4.0 a minute; after: the PAE frame-counter check, 1.0 a
              minute);
- short:      no radio, < 1 uC: the kits' short timer wakes; those 1.02 s
              apart (the "1.02 s wakes", not identified) are counted apart.
Prints per file: duration, mean current, floor (mean of the quiet samples),
windows per minute and their recurring periods, timer wakes per minute and their charge, sync-processing
wakes, short wakes per minute and the share on a 1.02 s grid. With --line,
one line per file for a campaign summary.
"""
import argparse
import json
import statistics as st
import sys


def classify(f):
    W = sorted(f["wake_list"], key=lambda w: w["t_s"])
    T = f["seconds"] or 1
    win, sync, timer, short = [], [], [], []
    last_win_end = -1e9
    for w in W:
        end = w["t_s"] + w["len_ms"] / 1000
        if (w.get("rx_ms") or 0) > 2 or (w.get("tx_ms") or 0) > 0:
            win.append(w)
            last_win_end = end
        elif w["q_uC"] >= 1:
            (sync if w["t_s"] - last_win_end <= 0.060 else timer).append(w)
        else:
            short.append(w)
    ts = [w["t_s"] for w in short]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    # on the 1.02 s grid: a gap within 10 ms of a multiple of 1.0195 s
    grid = sum(1 for g in gaps if g > 0.5 and abs(g / 1.0195 - round(g / 1.0195)) * 1.0195 < 0.010)
    m = T / 60
    # window periods: candidate spacings (any two windows up to 700 s apart,
    # rounded to 0.1 s) taken in increasing order; a spacing is a period if
    # it chains at least a third of the windows that no shorter period
    # chains yet (a window chained: other windows lie that far before or
    # after it, within 0.15 s, in a run of 4 windows g apart, 3 when the dump
    # is shorter than 4 g: a fixed offset between two schedules is not a
    # period). So 5 s unicast and 10 s broadcast slots of
    # another phase give [5, 10], not their multiples.
    wt = [w["t_s"] for w in win]
    cands = sorted({round(b - a, 1) for i, a in enumerate(wt) for b in wt[i + 1:] if 0.5 <= b - a <= 700})
    free = set(range(len(wt)))
    periods = []
    for g in cands:
        if len(free) < 2 or len(periods) == 3:
            break
        def at(t):
            return any(abs(u - t) <= 0.15 for u in wt)

        def run(t):   # length of the run of windows g apart through t
            n = 1
            for d in (g, -g):
                k = 1
                while at(t + k * d):
                    n += 1
                    k += 1
            return n
        need = min(4, max(3, int(T / g)))   # 4 windows in a run, 3 when the dump holds fewer
        ch = {i for i in free if run(wt[i]) >= need}
        if len(ch) >= 2 and len(ch) >= len(free) / 3:
            periods.append(g)
            free -= ch
    return {
        "window_periods_s": periods,
        "file": f["file"], "seconds": T, "mean_uA": f["mean_uA"], "floor_uA": f.get("floor_mean_uA"),
        "windows_per_min": round(len(win) / m, 2),
        "windows_tx": sum(1 for w in win if (w.get("tx_ms") or 0) > 0),
        "timer_per_min": round(len(timer) / m, 2),
        "timer_uC": round(st.median([w["q_uC"] for w in timer]), 2) if timer else None,
        "timer_uA": round(sum(w["q_uC"] for w in timer) / T, 3),
        "sync_proc": len(sync),
        "sync_uC": round(st.median([w["q_uC"] for w in sync]), 2) if sync else None,
        "short_per_min": round(len(short) / m, 1),
        "short_on_1020ms_grid": round(grid / len(gaps), 2) if gaps else None,
        "short_uA": round(sum(w["q_uC"] for w in short) / T, 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--line", action="store_true")
    a = ap.parse_args()
    for p in a.json:
        for f in json.load(open(p))["files"]:
            r = classify(f)
            if a.line:
                print(f"{r['file']}: {r['seconds']:.0f} s, mean {r['mean_uA']} uA, floor {r['floor_uA']} uA, "
                      f"windows {r['windows_per_min']}/min (periods {r['window_periods_s']} s), timer wakes {r['timer_per_min']}/min "
                      f"({r['timer_uC']} uC each, {r['timer_uA']} uA), sync processing {r['sync_proc']}, "
                      f"short wakes {r['short_per_min']}/min ({r['short_on_1020ms_grid']} on the 1.02 s grid, "
                      f"{r['short_uA']} uA)")
            else:
                print(json.dumps(r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
