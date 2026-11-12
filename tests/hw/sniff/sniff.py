#!/usr/bin/env python3
"""Preamble detection on the FG25 (RAILtest), for the emulator's preamble
sniffing model (emulator docs/MEDIUM.md §10). See docs/RADIO_TIMING_POWER_RESULTS.md.

Same bench as tests/hw/cochannel (R receiver, W wanted, I interferer, all
RAILtest): each cycle R sends a 10-byte trigger and turns its receiver on
X µs after the trigger's end (RAIL state timing txToRx = X, set with
setTimings), so the receiver comes on at a hardware-timed point of W's
frame. W answers the trigger with `txAfterRx d` and a frame of 50 bytes
whose preamble is LP bits long (alternate preamble length). I can send its
own frame over W's (its own d and length) as a co-channel interferer.

Per cycle, R's RAIL events RX_PREAMBLE_DETECT and RX_SYNC1_DETECT and its
received frames are recorded with R's timestamps relative to the trigger's
end; at most two or three preamble detections are printed per cycle (RAIL's
mode-3 PHY detects again about every 1 ms on a long preamble, and RAILtest's
event queue overflows above five), so only the first ones are used.

usage: sniff.py <exp> <out dir> --x X1,X2,...|START:STOP:STEP [--d 1000] [--lp 960] [--n 20]
                [--pW -150] [--pI P1,P2,...] [--dI 1000] [--ilen 250]
Every --pI value runs the X list; each point also runs W alone and I alone
cycles (one in ten each) for the RSSIs. Output <out>/<exp>.jsonl.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cochannel"))
from cochannel import Bench, MARK  # noqa: E402
from rt import parse  # noqa: E402

PD, SY = "RAIL_EVENT_RX_PREAMBLE_DETECT", "RAIL_EVENT_RX_SYNC1_DETECT"


def collect(b, rec, extra_s):
    R = b.k["R"]
    time.sleep(extra_s)
    lines = b.rstream + R.lines(0.05)
    b.rstream = []
    te = rec.get("trig_end")
    ev, rx = [], []
    for line in lines:
        m, d = parse(line)
        if te is None or not m:
            continue
        if m == "event" and "timestamp" in d:
            rel = (int(d["timestamp"]) - te) % (1 << 32)
            if rel < 60000:
                ev.append(["pd" if d.get("eventName") == PD else "sync" if d.get("eventName") == SY else d.get("eventName"), rel])
        elif m.startswith("rx") and m != "rx" and "timeUs" in d:
            rel = (int(d["timeUs"]) - te) % (1 << 32)
            p = d.get("payload", "").split()
            if rel < 60000:
                rx.append({"st": m, "t": rel, "crc": d.get("crc"), "rssi": int(d["rssi"]) if d.get("rssi") else None,
                           "mark": int(p[2], 16) if len(p) > 2 else None})
    rec["ev"], rec["rx"] = ev, rx
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp")
    ap.add_argument("out")
    ap.add_argument("--x", required=True, help="R's txToRx values (µs), comma separated")
    ap.add_argument("--d", type=int, default=1000)
    ap.add_argument("--lp", type=int, default=960, help="W's preamble (bits); 96 = the PHY's own")
    ap.add_argument("--wlen", type=int, default=50)
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--pW", type=int, default=-150)
    ap.add_argument("--pI", default="", help="I's powers (deci-dBm), comma separated; empty: no interferer")
    ap.add_argument("--dI", type=int, default=1000)
    ap.add_argument("--ilen", type=int, default=250)
    ap.add_argument("--alone", type=int, default=10, help="one W-alone and one I-alone cycle every N")
    a = ap.parse_args()
    b = Bench(a.out)
    b.setup()
    R, W = b.k["R"], b.k["W"]
    R.cmd("printEvents 0x60 0 0xFFFFFFFF 0xFFFFFFFF")
    R.cmd("resetCounters")
    b.set_frame("W", a.wlen, MARK["W"])
    b.set_power("W", a.pW)
    if a.lp != 96:
        W.cmd(f"setTxAltPreambleLen {a.lp}")
        W.cmd("configTxOptions 0x20")
    pis = [int(p) for p in a.pI.split(",") if p != ""] or [None]
    if pis[0] is not None:
        b.set_frame("I", a.ilen, MARK["I"])
    xs = []
    for part in a.x.split(","):
        if ":" in part:                      # start:stop:step
            lo, hi, st = map(int, part.split(":"))
            xs += list(range(lo, hi + 1, st))
        else:
            xs.append(int(part))
    frame_us = (a.lp / 8 + 2 + a.wlen + 4) * 8e6 / 150000
    extra = (max(xs + [a.d + frame_us, a.dI + (a.ilen + 20) * 8e6 / 150000]) + 3000) * 1e-6
    with open(os.path.join(a.out, f"{a.exp}.jsonl"), "a") as f:
        for pI in pis:
            if pI is not None:
                b.set_power("I", pI)
            for X in xs:
                R.cmd("rx 0")
                r = R.cmd(f"setTimings -1 {X} -1 -1 -1 -1 -1")
                got = [d.get("TxToRx") for m, d in r if m == "setTimings"]
                R.cmd("rx 1")
                for i in range(a.n):
                    kinds = [("WI" if pI is not None else "W")]
                    if pI is not None and i % a.alone == 0:
                        kinds = ["W", "I", "WI"]
                    for kind in kinds:
                        dW = a.d if "W" in kind else None
                        dI = a.dI if "I" in kind else None
                        rec = b.cycle(dW, dI)
                        collect(b, rec, extra)
                        rec.update(exp=a.exp, kind=kind, X=X, X_set=got[0] if got else None, d=a.d, lp=a.lp,
                                   wlen=a.wlen, pW=a.pW, pI=pI, dI_set=a.dI, ilen=a.ilen)
                        f.write(json.dumps(rec) + "\n")
                st = {k: v for m, d in R.cmd("status", 1.0) if m == "status" for k, v in d.items()
                      if k in ("RxCount", "SyncDetect", "NoRxBuffer", "RxCrcErrDrop", "FrameErrors", "Aborted")}
                R.cmd("resetCounters")
                f.write(json.dumps({"exp": a.exp, "kind": "status", "X": X, "pI": pI, "status": st}) + "\n")
                f.flush()
                print(a.exp, pI, X, st, flush=True)
        R.cmd("rx 0")
        R.cmd("setTimings -1 100 -1 -1 -1 -1 -1")


if __name__ == "__main__":
    main()
