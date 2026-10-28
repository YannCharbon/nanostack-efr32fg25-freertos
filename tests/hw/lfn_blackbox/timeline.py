#!/usr/bin/env python3
"""Merge both kits' PTI (host time) and the consoles into one timeline.

usage: timeline.py DIR T_START T_END [--all] [--ie] [--wsbrd]
Times are host unix seconds; output times are relative to T_START.
Without --all, the BR's periodic FFN frames (PA, PC, PAS, PCS, broadcast DATA) are left out.
"""
import argparse
import os
import subprocess
import sys

T = os.path.dirname(os.path.abspath(__file__))


def pti(path, who, ie):
    if not os.path.exists(path):
        return []
    cmd = [sys.executable, os.path.join(T, "pti_decode.py"), path] + (["--ie"] if ie else [])
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        f = line.split()
        try:
            t = float(f[0])
        except (ValueError, IndexError):
            continue
        rows.append((t, who, " ".join(f[2:])))
    return rows


def console(path, who):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, "rb").read().decode(errors="replace").splitlines():
        p = line.split(" ", 1)
        try:
            rows.append((float(p[0]), who, p[1] if len(p) > 1 else ""))
        except ValueError:
            pass
    return rows


def wsbrd_log(path, ie):
    """wsbrd's own trace lines ("<unix time>: text"); periodic PA/PC lines left out unless ie."""
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, "rb").read().decode(errors="replace").splitlines():
        p = line.split(": ", 1)
        try:
            t = float(p[0])
        except ValueError:
            continue
        if len(p) > 1:
            rows.append((t, "wsbrd", p[1]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("t0", type=float)
    ap.add_argument("t1", type=float)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--ie", action="store_true")
    ap.add_argument("--wsbrd", action="store_true", help="also wsbrd's trace (DIR/wsbrd.log)")
    a = ap.parse_args()
    rows = pti(os.path.join(a.dir, "br_dch.bin"), "BR-tx", a.ie) + pti(os.path.join(a.dir, "lfn_dch.bin"), "LFN-tx", a.ie)
    rows += console(os.path.join(a.dir, "lfn.log"), "LFN-con") + console(os.path.join(a.dir, "br.log"), "BR-con")
    if a.wsbrd:
        rows += wsbrd_log(os.path.join(a.dir, "wsbrd.log"), a.ie)
    rows.sort()
    for t, who, txt in rows:
        if not a.t0 <= t <= a.t1:
            continue
        if not a.all and who == "BR-tx" and any(f" {k} " in f" {txt} " for k in ("PA", "PC", "PAS", "PCS")):
            continue
        if not a.all and who == "BR-tx" and "->-" in txt and " DATA " in txt:
            continue
        if not a.all and who == "wsbrd" and any(k in txt for k in ("tx-15.4 adv", "tx-15.4 cfg", "drop tun", "rx-15.4 adv", "[DBG", "[INFO][rout")):
            continue
        print(f"{t - a.t0:10.3f} {who:8s} {txt}")


if __name__ == "__main__":
    main()
