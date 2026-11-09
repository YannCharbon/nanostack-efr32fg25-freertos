#!/usr/bin/env python3
"""Tables from stream.py results: frames received at R vs W-to-interferer ratio.

usage: analyze_stream.py <dir> [tag-glob]
SIR = W's mean packet RSSI alone - mean getRssi of the interferer alone on its
own channel (both read by R). For an adjacent interferer, -SIR is the
adjacent-channel ratio U/D.
"""
import glob
import json
import os
import sys


def main():
    d = sys.argv[1]
    pat = sys.argv[2] if len(sys.argv) > 2 else "*"
    for fn in sorted(glob.glob(os.path.join(d, pat + ".jsonl"))):
        rows = [json.loads(l) for l in open(fn)]
        rows = [r for r in rows if "mode" in r]
        if not rows:
            continue
        r0 = rows[0]
        print(f"\n### {os.path.basename(fn)[:-6]} ({r0['mode']}, interferer {r0['ioffset']:+d} channel, len {r0['len']})\n")
        print("| TX W / I (dBm) | RSSI W / I (dBm) | W/I (dB) | sent | received | CRC errors | syncs |")
        print("|---|---|---|---|---|---|---|")
        out = []
        for r in rows:
            mw = sum(r["rssiW"]) / len(r["rssiW"]) if r["rssiW"] else None
            mi = sum(r["rssiI"]) / len(r["rssiI"]) if r["rssiI"] else None
            sir = mw - mi if mw is not None and mi is not None else None
            out.append((sir if sir is not None else 99, r, mw, mi))
        for sir, r, mw, mi in sorted(out, key=lambda x: x[0]):
            pct = 100 * r["ok"] / r["w_tx"] if r["w_tx"] else float("nan")
            print(f"| {r['pW'] / 10:+.1f} / {r['pI'] / 10:+.1f} | {mw:.1f} / {mi:.1f} | {sir:+.1f} | {r['w_tx']} | "
                  f"{r['ok']} ({pct:.0f} %) | {r['r_crc']} | {r['r_sync']} |")


if __name__ == "__main__":
    main()
