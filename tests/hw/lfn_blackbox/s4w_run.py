#!/usr/bin/env python3
"""S4 with wsbrd: pings from the host, through wsbrd's TUN, to multicast groups (and unicast),
with random gaps so that requests fall at random phases of the LFN's schedule.

usage: s4w_run.py DIR GROUP [GROUP ...] [--n 5] [--timeout 30] [--tun tunlfn] [--tag X]
Each ping sends one echo request (`ping -6 -I TUN -c 4 -i 60 -w TIMEOUT GROUP`: one request,
then every reply until TIMEOUT; the host itself answers its own multicast at once). Every
replying address and its round-trip time are logged (DIR/s4_pings.json, and one line per ping in DIR/br.log for timeline.py).
"""
import argparse
import json
import os
import random
import re
import subprocess
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("groups", nargs="+")
    ap.add_argument("--n", type=int, default=5)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--tun", default="tunlfn")
    ap.add_argument("--gap-min", type=float, default=3)
    ap.add_argument("--gap-max", type=float, default=9)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    jf = os.path.join(a.dir, "s4_pings.json")
    res = json.load(open(jf)) if os.path.exists(jf) else []
    for i in range(a.n):
        for g in a.groups:
            t = time.time()
            with open(os.path.join(a.dir, "br.log"), "a") as f:
                f.write(f"{t:.3f} host ping {g}\n")
            o = subprocess.run(["ping", "-6", "-I", a.tun, "-c", "4", "-i", "60", "-w", str(a.timeout), g],
                               capture_output=True, text=True).stdout
            rep = [(m.group(1).rstrip(":"), float(m.group(2)))
                   for m in re.finditer(r"bytes from (\S+): icmp_seq=\d+ ttl=\d+ time=([\d.]+) ms", o)]
            r = {"tag": a.tag, "group": g, "i": i, "t_cmd": t, "replies": rep}
            res.append(r)
            print(f"{g} {i}: " + ", ".join(f"{f} {ms:.0f} ms" for f, ms in rep), flush=True)
            json.dump(res, open(jf, "w"), indent=1)
            time.sleep(random.uniform(a.gap_min, a.gap_max))


if __name__ == "__main__":
    main()
