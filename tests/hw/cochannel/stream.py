#!/usr/bin/env python3
"""Packet error rate of W's frames at R under a continuous interferer.

The interferer kit (I, or X = the 4th kit with --ikit X) sends a continuous
signal with RAILtest: an unmodulated carrier (`setTxTone`, the datasheet's
method) or a PN9 stream with the PHY's own modulation (`setTxStream 1`), on
R's channel (co-channel) or `--ioffset` channels away (adjacent). W sends
`--n` frames, `--gap` ms apart; R counts them with RAILtest's counters
(notifications off, so the console does not limit the rate):
received = RxCount - RxCrcErrDrop.

Levels at R: W's RSSI from 10 frames with nothing else on air; the
interferer's from R's RSSI reading (getRssi) on the interferer's channel
while it transmits and W is silent.

usage: stream.py <out dir> <tag> --mode tone|pn9 [--ioffset 0] --pairs "pW:pI,..."
"""
import argparse
import json
import os
import time

from cochannel import MARK, SN
from rt import RT, parse

SN = dict(SN, X=440235475)


def stat(k):
    for m, d in k.cmd("status", 0.5):
        if m == "status":
            return d
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("tag")
    ap.add_argument("--mode", choices=("tone", "pn9"), required=True)
    ap.add_argument("--ioffset", type=int, default=0)
    ap.add_argument("--channel", type=int, default=10)
    ap.add_argument("--len", type=int, default=50)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--gap", type=int, default=5)
    ap.add_argument("--ikit", default="I")
    ap.add_argument("--pairs", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "console.log")
    R, W, I = (RT(SN[n], log=log, name=n) for n in ("R", "W", a.ikit))
    for k in (R, W, I):
        k.cmd("reset", 1.5)
    time.sleep(0.3)
    for k in (R, W, I):
        k.lines(0.2)
        k.cmd("rx 0")
        k.cmd(f"setChannel {a.channel}")
    R.cmd("setRxOptions 10")
    W.cmd("setTxTransitions idle idle")
    W.cmd(f"setTxLength {a.len}")
    W.cmd("set802154PHR 1 0 1")
    W.cmd(f"setTxPayloadQuiet 2 {MARK['W']} {MARK['W']}")
    W.cmd(f"setTxDelay {a.gap}")
    I.cmd(f"setChannel {a.channel + a.ioffset}")
    fn = os.path.join(a.out, f"{a.tag}.jsonl")
    pl = [tuple(int(x) for x in p.split(":")) for p in a.pairs.split(",")]
    with open(fn, "a") as f:
        for pW, pI in pl:
            W.cmd("rx 0")
            gW = [d.get("power") for m, d in W.cmd(f"setPower {pW}") if m == "setPower"][0]
            I.cmd("rx 0")
            gI = [d.get("power") for m, d in I.cmd(f"setPower {pI}") if m == "setPower"][0]
            # W alone: RSSI
            R.cmd("setNotifications 1")
            R.cmd("rx 1")
            R.lines(0.05)
            W.cmd("tx 10", 0.2)
            time.sleep(0.15)
            rw = [int(d["rssi"]) for m, d in (parse(x) for x in R.lines(0.3))
                  if m == "rxPacket" and d.get("crc") == "Pass" and d.get("payload", "").split()[2:3] == ["0xaa"]]
            # interferer alone: RSSI on its channel
            R.cmd("rx 0")
            R.cmd(f"setChannel {a.channel + a.ioffset}")
            R.cmd("rx 1")
            I.cmd("setTxTone 1" if a.mode == "tone" else "setTxStream 1", 0.3)
            time.sleep(0.05)
            ri = []
            for _ in range(5):
                v = [d.get("rssi") for m, d in R.cmd("getRssi 1", 0.3) if m == "getRssi"]
                if v and v[0] not in (None, "Invalid"):
                    ri.append(float(v[0]))
            R.cmd("rx 0")
            R.cmd(f"setChannel {a.channel}")
            R.cmd("setNotifications 0")
            R.cmd("rx 1")
            time.sleep(0.02)
            R.cmd("resetCounters")
            W.cmd("resetCounters")
            W.lines(0.0)
            W.cmd(f"tx {a.n}", 0.2)
            t_end = time.time() + a.n * (a.gap + 4) / 1000 + 2
            done = False
            acc = []
            while time.time() < t_end and not done:
                for x in W.lines(0.05):
                    acc.append(x)
                    m, d = parse(x)
                    if m == "txEnd" and d.get("txRemain") == "0":
                        done = True
            time.sleep(0.05)
            sR, sW = stat(R), stat(W)
            I.cmd("setTxTone 0" if a.mode == "tone" else "setTxStream 0", 0.3)
            I.cmd("rx 0")
            rec = {"tag": a.tag, "mode": a.mode, "ioffset": a.ioffset, "ch": a.channel, "len": a.len,
                   "pW": int(gW), "pI": int(gI), "rssiW": rw, "rssiI": ri,
                   "w_tx": int(sW.get("UserTxCount", 0)), "r_rx": int(sR.get("RxCount", 0)),
                   "r_crc": int(sR.get("RxCrcErrDrop", 0)), "r_sync": int(sR.get("SyncDetect", 0)),
                   "r_frameerr": int(sR.get("FrameErrors", 0)), "t": round(time.time(), 1)}
            rec["ok"] = rec["r_rx"] - rec["r_crc"]
            f.write(json.dumps(rec) + "\n")
            f.flush()
            mw = sorted(rw)[len(rw) // 2] if rw else None
            mi = sorted(ri)[len(ri) // 2] if ri else None
            print(f"pW={gW} pI={gI} rssiW={mw} rssiI={mi} SIR={None if mw is None or mi is None else round(mw - mi, 1)} "
                  f"tx={rec['w_tx']} ok={rec['ok']} crc={rec['r_crc']} sync={rec['r_sync']}", flush=True)


if __name__ == "__main__":
    main()
