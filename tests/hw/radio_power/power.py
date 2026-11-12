#!/usr/bin/env python3
"""Steady-state currents and transitions of the FG25 under RAILtest, with the
kit's energy monitor (AEM), for the emulator's power profile (emulator
docs/POWER_MODEL.md §11). See docs/RADIO_TIMING_POWER_RESULTS.md.

The measured kit (--sn, a kit whose AEM sees the target: 440290182 on a
BRD4002A, 100 kHz; 440235509 on a BRD4001A, 10 kHz) runs RAILtest on the
mode-3 image. Each state is held while `commander aem dump` records it for
--dur seconds; the CSV is kept (gzip) and summarised (mean, median, 5th and
95th percentiles of the samples, supply voltage).

States (in order, any subset with --states):
  idle      radio idle (rx 0), MCU in RAILtest's main loop
  rx        receiver on, channel 0, nothing sent by the bench
  em1       `sleep 1`: EM1, radio idle      (woken by a CR afterwards)
  em2       `sleep 2`: EM2, radio off
  txP       continuous PN9 at P deci-dBm (setTxStream 1), e.g. tx-100, tx0,
            tx100, tx140, tx200 (the PA's maximum is what setPower returns)
  rxbusy    receiver on while --peer sends 250-byte frames 1 ms apart
  txpkt     50-byte frames from idle every 20 ms (tx 0 with setTxDelay), for
            the TX wake and teardown shapes
  rxtoggle  the host turns RX on and off every 50 ms (rx 1 / rx 0)
  chan      the host changes the channel in RX every 50 ms

usage: power.py --sn SN --out DIR [--peer SN] [--dur 1] [--states ...]
"""
import argparse
import csv
import gzip
import json
import os
import statistics as st
import subprocess
import sys
import threading
import time

RXBUSY_GAP_MS = int(os.environ.get("RXBUSY_GAP_MS", "1"))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cochannel"))
from rt import RT  # noqa: E402


def dump(sn, dur, path):
    r = subprocess.run(["commander", "aem", "dump", "-s", str(sn), "--duration", str(dur), "-o", path],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path):
        raise RuntimeError(f"aem dump {path}: {r.stdout[-300:]} {r.stderr[-300:]}")


def summary(path):
    t, i, v = [], [], []
    with open(path) as f:
        rd = csv.reader(f)
        next(rd)
        for row in rd:
            try:
                t.append(float(row[0])); i.append(float(row[1])); v.append(float(row[2]))
            except (ValueError, IndexError):
                pass
    s = sorted(i)
    pc = lambda p: s[min(len(s) - 1, int(p / 100 * len(s)))]
    return {"n": len(i), "dt_us": (t[-1] - t[0]) / (len(t) - 1), "mean_ma": st.fmean(i), "median_ma": pc(50),
            "p5_ma": pc(5), "p95_ma": pc(95), "v": st.fmean(v)}


def gz(path):
    with open(path, "rb") as fi, gzip.open(path + ".gz", "wb") as fo:
        fo.write(fi.read())
    os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sn", type=int, required=True)
    ap.add_argument("--peer", type=int)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dur", type=float, default=1)
    ap.add_argument("--states", default="idle,rx,em1,em2,tx-100,tx0,tx100,tx140,tx200,txpkt,rxtoggle,chan")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "console.log")
    k = RT(a.sn, log=log, name="P")
    k.cmd("reset", 1.5)
    k.lines(0.5)
    k.cmd("rx 0")
    k.cmd("setChannel 0")
    k.cmd("set802154PHR 1 0 1")
    k.cmd("setTxLength 50")
    res = {}
    out_json = os.path.join(a.out, "summary.json")
    if os.path.exists(out_json):
        res = json.load(open(out_json))
    for state in a.states.split(","):
        path = os.path.join(a.out, f"{a.sn}_{state}.csv")
        stop = threading.Event()
        bg = None
        info = {}
        if state == "idle":
            k.cmd("rx 0")
        elif state == "rx":
            k.cmd("rx 1")
        elif state in ("em1", "em2"):
            k.cmd("rx 0")
            k.send(f"sleep {state[2]}")
            time.sleep(0.3)
        elif state.startswith("tx") and state[2:].lstrip("-").isdigit():
            k.cmd("rx 0")
            r = k.cmd(f"setPower {int(state[2:])}")
            info["setPower"] = [d for m, d in r if m == "setPower"]
            k.cmd("setTxStream 1", 0.5)
        elif state == "rxbusy":
            p = RT(a.peer, log=log, name="peer")
            p.cmd("rx 0"); p.cmd("setChannel 0"); p.cmd("set802154PHR 1 0 1")
            p.cmd("setTxLength 250"); p.cmd("setPower -150"); p.cmd(f"setTxDelay {RXBUSY_GAP_MS}")      # ms
            k.cmd("rx 1")
            p.cmd("tx 0")
            time.sleep(0.2)
        elif state == "txpkt":
            k.cmd("rx 0")
            k.cmd("setPower 100")
            k.cmd("setTxTransitions idle idle")
            k.cmd("setTxDelay 20")         # ms (RAILtest: milliseconds)
            k.cmd("tx 0")
        elif state in ("rxtoggle", "chan"):
            k.cmd("rx 1" if state == "chan" else "rx 0")

            def toggler():
                on = False
                while not stop.is_set():
                    if state == "rxtoggle":
                        on = not on
                        k.send("rx 1" if on else "rx 0")
                    else:
                        on = not on
                        k.send("setChannel 17" if on else "setChannel 0")
                    time.sleep(0.05)
                    k.lines(0.0)
            bg = threading.Thread(target=toggler)
            bg.start()
        dump(a.sn, a.dur, path)
        stop.set()
        if bg:
            bg.join()
        # back to a known state
        if state in ("em1", "em2"):
            k.send("")
            time.sleep(0.3)
        if state.startswith("tx") and state[2:].lstrip("-").isdigit():
            k.cmd("setTxStream 0", 0.5)
        if state == "rxbusy":
            p.cmd("tx 0"); p.cmd("rx 0")
        if state == "txpkt":
            k.cmd("tx 0")
            k.cmd("setTxDelay 0")
        if state == "chan":
            k.cmd("setChannel 0")
        k.cmd("rx 0")
        k.lines(0.2)
        s = summary(path)
        s.update(info)
        res[state] = s
        print(state, json.dumps(s), flush=True)
        gz(path)
        json.dump(res, open(out_json, "w"), indent=1)


if __name__ == "__main__":
    main()
