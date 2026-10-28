#!/usr/bin/env python3
"""S5 parent restart with wsbrd: the LFN is joined and idle; wsbrd is stopped and restarted
at once with its storage kept (same keys, PAN ID and neighbour cache, as after a reboot);
then wait for the LFN's "[Connection lost" and "[Connected". wsbrd must already run in DIR
(wsbrd.py start DIR, or after s1w_run.py) with the LFN joined.

usage: s5w_run.py DIR [--reps 3] [--idle-s 60] [--down-s 0] [--timeout 900] [--extra FILE]
Results: DIR/s5_reps.json (host times; t_last_lts = last LFN sync frame (LTS, or broadcast LPC in its place) queued before the stop).
"""
import argparse
import json
import os
import re
import time

import wsbrd
from s1_run import size, wait


def last_lts(d, before):
    t = None
    for line in open(os.path.join(d, "wsbrd.log"), "rb").read().decode(errors="replace").splitlines():
        m = re.match(r"(\d+\.\d+): tx-15.4 (l-cfg|l-tsync)", line)
        if m and float(m.group(1)) < before:
            t = float(m.group(1))
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--idle-s", type=float, default=60)
    ap.add_argument("--down-s", type=float, default=0)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--extra")
    a = ap.parse_args()
    d = os.path.abspath(a.dir)
    jf = os.path.join(d, "s5_reps.json")
    res = json.load(open(jf)) if os.path.exists(jf) else []
    for rep in range(a.reps):
        time.sleep(a.idle_s)
        r = {"rep": len(res)}
        pos = size(d, "lfn")
        r["t_stop"] = time.time()
        r["t_last_lts"] = last_lts(d, r["t_stop"])
        wsbrd.stop_wsbrd(d)
        time.sleep(a.down_s)
        pcap, r["t_br_started"] = wsbrd.start_wsbrd(d, a.extra, fresh_storage=False)
        r["pcap"] = os.path.basename(pcap)
        if not r["t_br_started"]:
            print(f"rep {r['rep']}: wsbrd did not start (see wsbrd.log)", flush=True)
            break
        m = wait(d, "lfn", r"\[Connection lost", pos, a.timeout)
        r["t_lost"] = time.time() if m else None
        m = wait(d, "lfn", r"\[Connected: (\d+) s\]", pos, a.timeout) if m else None
        r["t_connected"] = time.time() if m else None
        res.append(r)
        json.dump(res, open(jf, "w"), indent=1)
        f = lambda k: f"{r[k] - r['t_stop']:.1f}" if r.get(k) else "-"
        print(f"rep {r['rep']}: last LTS {f('t_last_lts')} s, lost +{f('t_lost')} s, connected +{f('t_connected')} s",
              flush=True)


if __name__ == "__main__":
    main()
