#!/usr/bin/env python3
"""Charge of a short receive window on the FG25 (RAILtest's RX duty cycle):
the radio opens a window of ON us every PERIOD us by its own timer, with no
frame on air; the kit's energy monitor gives the mean current. The extra
charge per window over the idle radio is (mean - idle) x period.
usage: dutycycle.py --sn SN --out DIR [--on 150,300,500,1000] [--period 200000] [--dur 3]
"""
import argparse, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "cochannel"))
from power import dump, summary  # noqa: E402
from rt import RT  # noqa: E402
ap = argparse.ArgumentParser()
ap.add_argument("--sn", type=int, required=True); ap.add_argument("--out", required=True)
ap.add_argument("--on", default="150,300,500,1000"); ap.add_argument("--period", type=int, default=200000)
ap.add_argument("--dur", type=float, default=3)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
rt = RT(a.sn, log=os.path.join(a.out, "console.log"))
rt.cmd("rx 0"); rt.cmd("enableRxDutyCycle 0")
p = os.path.join(a.out, "idle.csv"); dump(a.sn, a.dur, p); idle = summary(p)["mean_ma"]
res = {"idle_ma": idle}
for on in [int(x) for x in a.on.split(",")]:
    rt.cmd("rx 0"); rt.cmd(f"configRxDutyCycle 1 {on} {a.period - on}"); rt.cmd("enableRxDutyCycle 1"); rt.cmd("rx 1")
    p = os.path.join(a.out, f"on{on}.csv"); dump(a.sn, a.dur, p); m = summary(p)["mean_ma"]
    res[on] = {"mean_ma": m, "uc_per_window": (m - idle) * a.period / 1000.0}
    print(on, json.dumps(res[on]), flush=True)
    rt.cmd("rx 0"); rt.cmd("enableRxDutyCycle 0")
print("idle_ma", idle)
json.dump(res, open(os.path.join(a.out, "dutycycle.json"), "w"), indent=1)
