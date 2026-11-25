#!/usr/bin/env python3
"""Capture and analysis helpers for the measurement firmware of this folder
(app.c: console line formats; docs/RADIO_TIMING_POWER_RESULTS.md section 12).

usage: fmlog.py cap SN SECONDS OUT [--reset]   log a kit's console lines
       fmlog.py edge LOG... [--frame-us 800]   role 1: reception interval
       fmlog.py turn LOG...                    roles 5/6: turnaround, carrier sense
       fmlog.py drift PERIOD_US LOG...         role 4: clock ratios
       fmlog.py wakes CSV...                   AEM dump: charge per wake
"""
import collections
import csv
import json
import statistics as st
import subprocess
import sys

DEV = "EFR32FG25B222F1920IM56"


def cap(sn, secs, out, reset=False):
    import serial
    import time
    s = serial.Serial(f"/dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_000{sn}-if00", 115200, timeout=0.5)
    if reset:
        subprocess.run(["commander", "device", "reset", "-s", str(sn), "-d", DEV], capture_output=True)
    t_end = time.time() + secs
    with open(out, "w") as f:
        while time.time() < t_end:
            line = s.readline().decode(errors="replace")
            if line:
                f.write(line)
    s.close()


def records(path, tag):
    out = []
    for line in open(path, errors="replace"):
        x = line.split()
        if "fmmeas role" in line:      # the kit restarted: keep what follows
            out = []
        if len(x) == 8 and x[0] == tag:
            try:
                out.append([int(v) for v in x[1:]])
            except ValueError:
                pass
    return out


def edge(path, frame_us=800):
    """Role 1: the preamble start of each received frame against its window's
    opening (E - W - frame_us, E = the radio's frame-end timestamp)."""
    wins = 0
    for line in open(path, errors="replace"):
        wins = 0 if "fmmeas role" in line else wins + line.startswith("W ")
    R = records(path, "R")
    off = sorted(r[1] - frame_us for r in R)
    sync = sorted(r[4] for r in R)
    return {"windows": wins, "frames": len(R),
            "pos_dur": collections.Counter((r[2], r[3]) for r in R).most_common(2),
            "preamble_start_min_us": off[:3], "preamble_start_max_us": off[-3:],
            "sync_event_minus_end_us": collections.Counter(r[4] - r[1] for r in R).most_common(3),
            "sync_event_vs_window_us": [sync[:1], sync[-1:]],
            "per_20us": sorted(collections.Counter(20 * (o // 20) for o in off).items())}


def turn(path):
    """Role 5 (A lines): P-E, T-E per answer; role 6 (I lines): the answer's end
    against the initiator's own frame end."""
    out = {}
    A = records(path, "A")
    if A:
        ok = [a for a in A if a[1] == 0]
        out["answers"] = len(A)
        out["status"] = dict(collections.Counter(a[1] for a in A))
        out["tx_preamble_minus_rx_end_us"] = collections.Counter(a[2] for a in ok).most_common(3)
        out["tx_end_minus_tx_preamble_us"] = collections.Counter(a[3] - a[2] for a in ok).most_common(2)
        out["callback_minus_rx_end_us"] = collections.Counter(a[5] for a in A).most_common(2)
    Ii = records(path, "I")
    if Ii:
        out["replies"] = len(Ii)
        out["reply_end_minus_own_end_us"] = collections.Counter(a[1] for a in Ii).most_common(3)
        out["pos_txpos"] = collections.Counter((a[4], a[5]) for a in Ii).most_common(1)
    return out


def _fit(x, y):
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x)
    b = sum((a - mx) * (c - my) for a, c in zip(x, y)) / sxx
    a0 = my - b * mx
    res = [c - (a0 + b * a) for a, c in zip(x, y)]
    s = (sum(r * r for r in res) / (n - 2)) ** 0.5
    return b, s / sxx ** 0.5, s


def _unwrap(v, mod=1 << 32):
    out, off = [v[0]], 0
    for p, c in zip(v, v[1:]):
        if c < p - mod // 2:
            off += mod
        out.append(c + off)
    return out


def drift(period_us, path):
    """Role 4 receiving a role-2 sender of period period_us. sender_ppm > 0: the
    sender's clock is slow against the receiver's radio clock (crystal, HFXO).
    sleep_ppm > 0: the receiver's sleep clock (LFXO via the sleep timer) is slow
    against its own radio clock."""
    F = records(path, "F")
    E = _unwrap([r[0] & 0xffffffff for r in F])
    k = [round((e - E[0]) / period_us) for e in E]
    b, sb, s = _fit(k, E)
    N = [e + r[5] for e, r in zip(E, F)]
    L = _unwrap([r[6] & 0xffffffff for r in F])
    nom = 1e6 / 32768
    b2, sb2, s2 = _fit(L, N)
    h = len(k) // 2
    halves = [round((_fit(k[:h], E[:h])[0] / period_us - 1) * 1e6, 3), round((_fit(k[h:], E[h:])[0] / period_us - 1) * 1e6, 3)]
    return {"frames": len(F), "seconds": round((E[-1] - E[0]) / 1e6, 1), "missing": k[-1] + 1 - len(k),
            "sender_ppm": round((b / period_us - 1) * 1e6, 3), "sender_ppm_sd": round(sb / period_us * 1e6, 3),
            "sender_ppm_halves": halves,
            "resid_rms_us": round(s, 2),
            "sleep_ppm": round((b2 / nom - 1) * 1e6, 3), "sleep_ppm_sd": round(sb2 / nom * 1e6, 3)}


def wakes(path, thr_ma=0.1, pad_us=400):
    """AEM dump (commander aem dump CSV: us, mA, V): the floor (mean of the
    samples outside any activity) and the charge above it of each wake (peak
    above 0.5 mA); smaller events (peak at most 0.5 mA) are counted apart."""
    t, i = [], []
    with open(path) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            try:
                t.append(float(row[0]))
                i.append(float(row[1]))
            except (ValueError, IndexError):
                pass
    dt = (t[-1] - t[0]) / (len(t) - 1)
    w = max(1, int(300 / dt))
    sm, acc = [], 0.0
    for k, x in enumerate(i):
        acc += x
        if k >= w:
            acc -= i[k - w]
        sm.append(acc / min(k + 1, w))
    n, runs, k = len(i), [], 0
    while k < n:
        if sm[k] > thr_ma:
            j = k
            while j < n and sm[j] > thr_ma:
                j += 1
            a, b = max(0, k - w - int(pad_us / dt)), min(n, j + int(pad_us / dt))
            if runs and a <= runs[-1][1] + int(1000 / dt):
                runs[-1][1] = b
            else:
                runs.append([a, b])
            k = j
        else:
            k += 1
    mask = [False] * n
    for a, b in runs:
        for k in range(a, b):
            mask[k] = True
    floor = st.fmean([x for x, m in zip(i, mask) if not m])
    runs = [r for r in runs if r[0] > 0 and r[1] < n]
    wk = [r for r in runs if max(i[r[0]:r[1]]) > 0.5]
    bl = [r for r in runs if max(i[r[0]:r[1]]) <= 0.5]
    q = [sum(x - floor for x in i[a:b]) * dt / 1000.0 for a, b in wk]
    secs = (t[-1] - t[0]) / 1e6
    mean = st.fmean(i)
    return {"seconds": round(secs, 2), "mean_uA": round(mean * 1000, 2), "floor_uA": round(floor * 1000, 2),
            "wakes_per_s": round(len(wk) / secs, 3),
            "q_uC": round(st.median(q), 3) if q else None, "q_sd_uC": round(st.pstdev(q), 3) if q else None,
            "small_per_s": round(len(bl) / secs, 2),
            "small_uC": round(st.fmean([sum(x - floor for x in i[a:b]) * dt / 1000.0 for a, b in bl]), 3) if bl else 0}


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "cap":
        cap(args[0], float(args[1]), args[2], "--reset" in args)
    elif cmd == "edge":
        fu = 800
        if "--frame-us" in args:
            fu = int(args[args.index("--frame-us") + 1])
            args = [a for a in args if a not in ("--frame-us", str(fu))]
        for p in args:
            print(p, json.dumps(edge(p, fu)))
    elif cmd == "turn":
        for p in args:
            print(p, json.dumps(turn(p)))
    elif cmd == "drift":
        for p in args[1:]:
            print(p, json.dumps(drift(float(args[0]), p)))
    elif cmd == "wakes":
        for p in args:
            print(p, json.dumps(wakes(p)))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
