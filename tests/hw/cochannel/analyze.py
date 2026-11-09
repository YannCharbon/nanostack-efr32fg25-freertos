#!/usr/bin/env python3
"""Tables from the sweep results (<dir>/*.jsonl written by sweep.py).

For each file and power point: RSSI at R of W alone and of I alone (median
of the solo cycles), SIR = RSSI_W - RSSI_I, number of valid two-frame
cycles, and the outcome counts: W received, I received, neither, both
(cannot happen with overlapping frames), plus the solo baselines.
I is the first frame, W starts `delta` us later.

usage: analyze.py <dir> [tag-glob]   -> prints Markdown tables; --csv for CSV
"""
import glob
import json
import math
import os
import sys


def wilson(k, n, z=1.96):
    if n == 0:
        return (float("nan"), float("nan"))
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))


def med(v):
    v = sorted(v)
    if not v:
        return None
    m = len(v) // 2
    return v[m] if len(v) % 2 else (v[m - 1] + v[m]) / 2


def points(fn):
    pts = {}
    for line in open(fn):
        r = json.loads(line)
        key = (r["pW"], r["pI"])
        p = pts.setdefault(key, {"rW": [], "rI": [], "both": [], "soloW": [], "soloI": [], "inval": 0, "meta": r})
        if not r["valid"]:
            p["inval"] += 1
            continue
        res = r["res"]
        if r["dW"] is not None and r["dI"] is not None:
            p["both"].append(res)
        elif r["dW"] is not None:
            p["soloW"].append(res["W"])
            p["rW"] += [f["rssi"] for f in r["rx"] if f["crc"] == "Pass" and f["p"][2:3] == [0xAA]]
        else:
            p["soloI"].append(res["I"])
            p["rI"] += [f["rssi"] for f in r["rx"] if f["crc"] == "Pass" and f["p"][2:3] == [0xBB]]
    out = []
    for (pW, pI), p in pts.items():
        mean = lambda v: round(sum(v) / len(v), 1) if v else None
        rW, rI = mean(p["rW"]), mean(p["rI"])
        n = len(p["both"])
        kW = sum(x["W"] == "ok" for x in p["both"])
        kI = sum(x["I"] == "ok" for x in p["both"])
        kB = sum(x["W"] == "ok" and x["I"] == "ok" for x in p["both"])
        out.append({
            "pW": pW, "pI": pI, "rssiW": rW, "rssiI": rI,
            "sir": round(rW - rI, 1) if rW is not None and rI is not None else None,
            "n": n, "W_ok": kW, "I_ok": kI, "none": n - kW - kI + kB, "both_ok": kB,
            "W_bad": sum(x["W"] == "bad" for x in p["both"]), "I_bad": sum(x["I"] == "bad" for x in p["both"]),
            "soloW": f"{sum(x == 'ok' for x in p['soloW'])}/{len(p['soloW'])}",
            "soloI": f"{sum(x == 'ok' for x in p['soloI'])}/{len(p['soloI'])}",
            "invalid": p["inval"], "delta": p["meta"]["delta"], "len": p["meta"]["wlen"],
        })
    out.sort(key=lambda x: (x["sir"] if x["sir"] is not None else 99))
    return out


def main():
    d = sys.argv[1]
    pat = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "*"
    csv = "--csv" in sys.argv
    for fn in sorted(glob.glob(os.path.join(d, pat + ".jsonl"))):
        pts = points(fn)
        if not pts:
            continue
        print(f"\n### {os.path.basename(fn)[:-6]}  (len {pts[0]['len']}, I first, W +{pts[0]['delta']} us)\n")
        if csv:
            print("pW,pI,rssiW,rssiI,sir,n,W_ok,I_ok,none,W_bad,I_bad,soloW,soloI,invalid")
            for p in pts:
                print(",".join(str(p[k]) for k in ("pW", "pI", "rssiW", "rssiI", "sir", "n", "W_ok", "I_ok", "none",
                                                     "W_bad", "I_bad", "soloW", "soloI", "invalid")))
            continue
        print("| TX W / I (dBm) | RSSI W / I (dBm) | SIR of W (dB) | cycles | W (2nd) received | I (1st) received | neither | W alone | I alone |")
        print("|---|---|---|---|---|---|---|---|---|")
        for p in pts:
            fW = p["W_ok"] / p["n"] if p["n"] else float("nan")
            fI = p["I_ok"] / p["n"] if p["n"] else float("nan")
            print(f"| {p['pW'] / 10:+.1f} / {p['pI'] / 10:+.1f} | {p['rssiW']} / {p['rssiI']} | {p['sir']:+} | {p['n']} | "
                  f"{p['W_ok']} ({100 * fW:.0f} %) | {p['I_ok']} ({100 * fI:.0f} %) | {p['none']} | {p['soloW']} | {p['soloI']} |")


if __name__ == "__main__":
    main()
