#!/usr/bin/env python3
"""S3/S4 pings through the Silicon Labs CLIs (consoles from session.sh must run).

usage: s3_run.py DIR --from br|lfn --to ADDR [--n 10] [--timeout 30] [--gap-min 3] [--gap-max 9]
Each ping: one `wisun ping ADDR`, wait for the reply line or the timeout, then a random gap
(so that the requests fall at random phases of the LFN's listening schedule).
Results are appended to DIR/s3_pings.json.
"""
import argparse
import json
import os
import random
import re
import time


def size(p):
    return os.path.getsize(p) if os.path.exists(p) else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--from", dest="src", choices=["br", "lfn"], required=True)
    ap.add_argument("--to", required=True)
    ap.add_argument("--n", type=int, default=10)
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--gap-min", type=float, default=3)
    ap.add_argument("--gap-max", type=float, default=9)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    log = os.path.join(a.dir, f"{a.src}.log")
    jf = os.path.join(a.dir, "s3_pings.json")
    res = json.load(open(jf)) if os.path.exists(jf) else []
    rx = re.compile(r"bytes from \S+: icmp_seq=(\d+) time=(\d+) ms")
    for i in range(a.n):
        pos = size(log)
        t = time.time()
        with open(os.path.join(a.dir, f"{a.src}.fifo"), "w") as f:
            f.write(f"wisun ping {a.to}\n")
        rtt = None
        while time.time() - t < a.timeout:
            with open(log, "rb") as f:
                f.seek(pos)
                m = rx.search(f.read().decode(errors="replace"))
            if m:
                rtt = int(m.group(2))
                break
            time.sleep(0.2)
        r = {"tag": a.tag, "from": a.src, "to": a.to, "i": i, "t_cmd": t, "rtt_ms": rtt}
        res.append(r)
        print(f"{a.src}->{a.to} {i}: {rtt} ms", flush=True)
        json.dump(res, open(jf, "w"), indent=1)
        time.sleep(random.uniform(a.gap_min, a.gap_max))


if __name__ == "__main__":
    main()
