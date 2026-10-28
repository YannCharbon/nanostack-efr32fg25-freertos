#!/usr/bin/env python3
"""Summarise hw5_power.py runs: firmware statistics against the AEM average.

usage: tests/hw/pstats_parse.py RUN_DIR [--elf ELF] [--json OUT]

The datasheet model (EFR32FG25 datasheet rev 1.2, DC-DC, typical):
  EM0 at 97.5 MHz      31 uA/MHz (Table 4.5, Prime from flash)  -> 3.02 mA
  EM1 at 97.5 MHz      22 uA/MHz (Table 4.5)                    -> 2.15 mA
  RX 868 MHz 2-GFSK    5.7 mA with the MCU in EM1 at 39 MHz (Table 4.8);
                       radio part = 5.7 - 26 uA/MHz * 39 MHz = 4.69 mA
  TX +10 dBm 868 MHz   not in the table; 0 dBm 24.9 mA, 12 dBm 54.5 mA,
                       14 dBm @868 52.0 mA (915 MHz CW): about 45 mA assumed,
                       radio part 45 - 1.01 = 44 mA
Sum = t_EM0/T*I_EM0 + t_EM1/T*I_EM1 + t_RX/T*I_RX_radio + t_TX/T*I_TX_radio.
"""
import argparse
import json
import os
import re
import subprocess
import statistics as stx

I_EM0 = 0.031 * 97.5
I_EM1 = 0.022 * 97.5
I_RX = 5.7 - 0.026 * 39.0
I_TX = 45.0 - 0.026 * 39.0


def parse_block(text):
    """Return the dict of the first complete 'ps ... ps end' block in text."""
    d = {"cause": {}, "cbt": {}, "kind": {}, "em": {}, "radio": {}}
    started = False
    for line in text.splitlines():
        m = re.search(r"\bps (.*)$", line)
        if not m:
            continue
        f = m.group(1).split()
        if f[0] == "elapsed_us":
            started = True
            d.update(elapsed_us=int(f[1]), cpu_hz=int(f[3]), cycles=int(f[5]), wakes=int(f[7]))
        elif not started:
            continue
        elif f[0] == "end":
            return d
        elif f[0] == "em":
            d["em"][int(f[1])] = (int(f[3]), int(f[5]))
        elif f[0] == "wake":
            d["wake"] = (int(f[2]), int(f[4]), int(f[6]))
        elif f[0] == "cause":
            d["cause"][f[1]] = (int(f[3]), int(f[5]), int(f[7]))
        elif f[0] == "cbt":
            d["cbt"][int(f[1])] = {"fn": f[3], "n": int(f[5]), "cyc": int(f[7]), "max": int(f[9]),
                                   "primary": int(f[11]), "alone": int(f[13])}
        elif f[0] == "kind":
            d["kind"][f[1]] = (int(f[3]), int(f[5]), int(f[7]))
        elif f[0] == "work":
            d["work"] = {f[i]: int(f[i + 1]) for i in range(1, len(f) - 1, 2)}
        elif f[0] == "radio":
            d["radio"][f[1]] = (int(f[3]), int(f[5]))
        elif f[0] == "frames":
            d["frames"] = {f[i]: int(f[i + 1]) for i in range(1, len(f) - 1, 2)}
    return None


def symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    sym = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tT":
            sym[int(p[0], 16) & ~1] = p[2]
    return sym


def window_figures(d):
    T = d["elapsed_us"]
    em0, em1 = d["em"][0][0], d["em"][1][0]
    rx, tx = d["radio"]["rx"][0], d["radio"]["tx"][0]
    model = (em0 * I_EM0 + em1 * I_EM1 + rx * I_RX + tx * I_TX) / T
    return {
        "T_s": T / 1e6, "em0_frac": em0 / T, "em1_frac": em1 / T, "em2_frac": d["em"][2][0] / T,
        "em0_entries_per_s": d["em"][0][1] / (T / 1e6),
        "wakes_per_s": d["wakes"] / (T / 1e6),
        "cyc_per_wake": d["wake"][1] / max(1, d["wake"][0]),
        "rx_frac": rx / T, "tx_frac": tx / T, "idle_frac": d["radio"]["idle"][0] / T,
        "rx_frames_per_s": d["frames"]["rx"] / (T / 1e6), "tx_frames_per_s": d["frames"]["tx"] / (T / 1e6),
        "rx_air_frac": d["frames"]["rx_air_us"] / T, "tx_air_frac": d["frames"]["tx_air_us"] / T,
        "model_ma": model,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--elf")
    ap.add_argument("--json")
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.run, "meta.json")))
    log = open(os.path.join(a.run, f"meas_{meta['meas_role']}.log"), "rb").read().decode(errors="replace")
    sym = symbols(a.elf) if a.elf else {}
    res = {"meas_role": meta["meas_role"], "modes": {}}
    for mode in sorted({w["mode"] for w in meta["windows"]}):
        ws = [w for w in meta["windows"] if w["mode"] == mode]
        rows, blocks = [], []
        for w in ws:
            d = parse_block(log[w["log_offset"]:]) if w["aem_ma"] is not None else None
            if not d:
                continue
            f = window_figures(d)
            f["aem_ma"] = w["aem_ma"]
            f["gap_ma"] = w["aem_ma"] - f["model_ma"]
            rows.append(f)
            blocks.append(d)
        if not rows:
            continue
        keys = rows[0].keys()
        summ = {k: (stx.mean(r[k] for r in rows), stx.pstdev(r[k] for r in rows)) for k in keys}
        # Pool the cycle accounting over the windows of this mode.
        pooled = {"cause": {}, "kind": {}, "cbt": {}}
        for d in blocks:
            for c, (n, cyc, mx) in d["cause"].items():
                p = pooled["cause"].setdefault(c, [0, 0, 0])
                p[0] += n; p[1] += cyc; p[2] = max(p[2], mx)
            for c, (n, cyc, mx) in d["kind"].items():
                p = pooled["kind"].setdefault(c, [0, 0, 0])
                p[0] += n; p[1] += cyc; p[2] = max(p[2], mx)
            for i, c in d["cbt"].items():
                name = sym.get(int(c["fn"], 16) & ~1, c["fn"])
                p = pooled["cbt"].setdefault(name, {"n": 0, "cyc": 0, "max": 0, "primary": 0, "alone": 0})
                for k in ("n", "cyc", "primary", "alone"):
                    p[k] += c[k]
                p["max"] = max(p["max"], c["max"])
        T = sum(r["T_s"] for r in rows)
        res["modes"][mode] = {"windows": len(rows), "summary": summ, "pooled": pooled, "T_s": T, "rows": rows}
        print(f"\n=== {meta['meas_role'].upper()} {mode}: {len(rows)} windows, {T:.0f} s")
        for k in keys:
            m, s = summ[k]
            print(f"  {k:22s} {m:12.5g}  sd {s:.3g}")
        print("  wake cause            n/s     cyc/wake    max cyc")
        for c, (n, cyc, mx) in sorted(pooled["cause"].items(), key=lambda x: -x[1][0]):
            if n:
                print(f"    {c:16s} {n / T:8.2f} {cyc / n:12.0f} {mx:10d}")
        print("  work kind             n/s     cyc/call    max cyc")
        for c, (n, cyc, mx) in pooled["kind"].items():
            if n:
                print(f"    {c:16s} {n / T:8.2f} {cyc / n:12.0f} {mx:10d}")
        print("  callback timer                         n/s   cyc/call  primary/s  alone/s")
        for name, p in sorted(pooled["cbt"].items(), key=lambda x: -x[1]["n"]):
            print(f"    {name:34s} {p['n'] / T:8.2f} {p['cyc'] / max(1, p['n']):9.0f} {p['primary'] / T:9.2f} {p['alone'] / T:8.2f}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
