#!/usr/bin/env python3
"""Per-repetition S1 milestones from the merged timeline (PTI of both kits + consoles).

usage: s1_stats.py DIR
"""
import json
import os
import re
import statistics as st
import subprocess
import sys

T = os.path.dirname(os.path.abspath(__file__))


def main():
    d = sys.argv[1]
    reps = json.load(open(os.path.join(d, "s1_reps.json")))
    keys = ["sweeps", "lpa", "auth", "eapol1", "pcfg", "lpcs1", "n_lpcs", "lpc", "dhcp", "dhcp_sol", "areg",
            "oper", "lus_ms"]
    rows = []
    for r in reps:
        if not r.get("t_connected"):
            continue
        t0 = r["t_join"]
        out = subprocess.run([sys.executable, os.path.join(T, "timeline.py"), d, str(t0), str(r["t_connected"] + 2),
                              "--ie"], capture_output=True, text=True).stdout
        lines = out.splitlines()

        def first(pat):
            for l in lines:
                if re.search(pat, l):
                    return float(l.split()[0])
            return None
        lp = [float(l.split()[0]) for l in lines if " LPAS " in l]
        sweeps = 1 + sum(1 for a, b in zip(lp, lp[1:]) if b - a > 2) if lp else 0
        lpcs = [float(l.split()[0]) for l in lines if "LFN-tx" in l and " LPCS " in l]
        m = re.search(r"LUS=([0-9a-f]{6})", out)
        x = {
            "rep": r["rep"], "sweeps": sweeps,
            "lpa": first(r"BR-tx .* LPA "),
            "auth": first(r"Join state 2"),
            "eapol1": first(r"LFN-tx .* EAPOL "),
            "pcfg": first(r"Join state 3"),
            "lpcs1": lpcs[0] if lpcs else None, "n_lpcs": len(lpcs),
            "lpc": first(r"BR-tx .* LPC "),
            "dhcp": first(r"Join state 4: Configure IP Layer - DHCP"),
            "dhcp_sol": None,
            "areg": first(r"address registration"),
            "oper": first(r"Join state 5"),
            "lus_ms": int.from_bytes(bytes.fromhex(m.group(1)), "little") if m else None,
        }
        if x["dhcp"]:
            for l in lines:
                t = float(l.split()[0])
                if t > x["dhcp"] and "LFN-tx" in l and " DATA " in l:
                    x["dhcp_sol"] = t
                    break
        rows.append(x)
    print("rep " + " ".join(f"{k:>8s}" for k in keys))
    for x in rows:
        print(f"{x['rep']:3d} " + " ".join(f"{x[k]:8.2f}" if isinstance(x[k], float) else f"{str(x[k]):>8s}"
                                          for k in keys))
    for k in keys:
        v = [x[k] for x in rows if isinstance(x[k], (int, float)) and x[k] is not None]
        if v:
            print(f"  {k:9s} median {st.median(v):8.2f}  min {min(v):8.2f}  max {max(v):8.2f}  n {len(v)}")
    # phase durations
    def dur(a, b):
        v = [x[b] - x[a] for x in rows if x[a] is not None and x[b] is not None]
        return (st.median(v), min(v), max(v), len(v)) if v else None
    for a, b, name in [("lpa", "auth", "LPA -> Authenticate state"), ("auth", "eapol1", "Authenticate -> 1st EAPOL"),
                       ("eapol1", "pcfg", "1st EAPOL -> PAN config state"), ("lpcs1", "lpc", "1st LPCS -> LPC"),
                       ("dhcp", "dhcp_sol", "DHCP state -> 1st DATA (Solicit)"), ("dhcp_sol", "areg", "Solicit -> addr reg"),
                       ("areg", "oper", "addr reg -> Operational")]:
        r = dur(a, b)
        if r:
            print(f"  {name:34s} median {r[0]:6.2f}  min {r[1]:6.2f}  max {r[2]:6.2f}  n {r[3]}")
    json.dump(rows, open(os.path.join(d, "s1_stats.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
