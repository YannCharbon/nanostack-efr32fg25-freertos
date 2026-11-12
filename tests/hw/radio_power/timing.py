#!/usr/bin/env python3
"""Radio start-up and switching times on the FG25 (RAILtest), from RAIL's
own timestamps. See docs/RADIO_TIMING_POWER_RESULTS.md.

W sends one frame whose preamble is very long (--lp bits, default 30000:
200 ms at 150 kbit/s), so the host's timing does not matter. While it is on
air, R, the receiver, goes from idle (rx 0) to RX (`rx 1`), for several
idleToRx settings (setTimings), and the time from the command's RAIL timestamp (the {Time:} of RAILtest's
answer, taken when RAILtest handles the command, i.e. just before or after
it calls RAIL) to R's first RX_PREAMBLE_DETECT event is recorded.
Also, on W alone: `tx 1` from idle and from RX, the time from the command's
timestamp to RAIL's TX start (txEnd lastTxStart), for several idleToTx /
rxToTx settings (setTimings), to separate the setting from the software.

usage: timing.py <out dir> [--n 20]
Output <out>/timing.jsonl, console log <out>/console.log
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cochannel"))
from cochannel import SN  # noqa: E402
from rt import RT, parse  # noqa: E402

PD = "RAIL_EVENT_RX_PREAMBLE_DETECT"


def resp_time(rs, name):
    for m, d in rs:
        if m == name and "Time" in d:
            return int(d["Time"])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--lp", type=int, default=30000)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "console.log")
    R, W = RT(SN["R"], log=log, name="R"), RT(SN["W"], log=log, name="W")
    for k in (R, W):
        k.cmd("reset", 1.5)
    time.sleep(0.3)
    for k in (R, W):
        k.lines(0.3)
        k.cmd("rx 0"); k.cmd("setChannel 0"); k.cmd("set802154PHR 1 0 1")
        k.cmd("setTxTransitions idle idle"); k.cmd("setRxTransitions idle idle")
    R.cmd("printEvents 0x20 0 0xFFFFFFFF 0xFFFFFFFF")
    W.cmd("setTxLength 20"); W.cmd("setPower -150")
    W.cmd(f"setTxAltPreambleLen {a.lp}"); W.cmd("configTxOptions 0x20")
    f = open(os.path.join(a.out, "timing.jsonl"), "a")
    for i2r in (100, 300, 1000):
        R.cmd(f"setTimings {i2r} -1 -1 -1 -1 -1 -1")
        for i in range(a.n):
            R.cmd("rx 0")
            R.lines(0.05)
            W.cmd("tx 1", 0.1)
            time.sleep(0.03)          # W's preamble on air (it lasts lp/150 ms)
            rs = R.cmd("rx 1", 0.2)
            t_cmd = resp_time(rs, "rx")
            lines = R.lines(a.lp / 150000 + 0.1)
            pds = [int(d["timestamp"]) for m, d in rs + [parse(l) for l in lines]
                   if m == "event" and d.get("eventName") == PD]
            f.write(json.dumps({"exp": "rxstart", "setting": i2r, "t_cmd": t_cmd, "pd": pds}) + "\n")
            f.flush()
            W.lines(0.05)
        print("rx idle", i2r, flush=True)
    R.cmd("rx 0"); R.cmd("setChannel 0")
    # TX start latency on W
    W.cmd("configTxOptions 0")
    for state in ("idle", "rx"):
        for i2t in (100, 300, 1000):
            if state == "idle":
                W.cmd(f"setTimings -1 -1 {i2t} -1 -1 -1 -1")
            else:
                W.cmd(f"setTimings -1 -1 -1 {i2t} -1 -1 -1")
            for i in range(a.n):
                W.cmd("rx 1" if state == "rx" else "rx 0")
                W.lines(0.05)
                rs = W.cmd("tx 1", 0.3, until="txEnd")
                t_cmd = resp_time(rs, "tx")
                st_ = None
                for m, d in rs:
                    if m == "txEnd":
                        st_ = int(d.get("lastTxStart", 0))
                f.write(json.dumps({"exp": "txstart", "state": state, "setting": i2t, "t_cmd": t_cmd,
                                    "tx_start": st_}) + "\n")
            f.flush()
            print("tx", state, i2t, flush=True)
    W.cmd("rx 0"); W.cmd("setTimings 120 100 120 130 -1 -1 -1")


if __name__ == "__main__":
    main()
