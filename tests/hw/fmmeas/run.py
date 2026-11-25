#!/usr/bin/env python3
"""Measurements of the fire alarm mesh stack's radio on the FG25 kits, with
the measurement firmware of this folder (build.sh, app.c). Results as JSON in
OUT/results.json; docs/RADIO_TIMING_POWER_RESULTS.md section 12.

usage: run.py OUT [--parts power,timing,turn,drift] [--meter 440235509]
              [--rx 440235475] [--tx 440240046] [--wins 0,100,200,300,600]
              [--slcp fmmeas_noio.slcp] [--conf "NAME=VALUE ..."]
              [--defs "-DFM_TSYNC=1 -DFM_PMOVH=-12"] [--dur 6] [--secs 60]

power:  on the kit with the energy monitor (--meter), one image per window
        length (--wins; 0: no radio, the floor), built with --slcp, --conf
        and --defs; per image the floor and the median charge of a listening
        moment above it (fmlog.py wakes); a line fitted against the window
        length gives the fixed cost and the current while the receiver is on.
        Default build: the recommended low-power settings (no console, no
        debugging in deep sleep, RAIL's timer kept across deep sleep, the
        power manager's early wake 12 sleep-timer ticks shorter).
timing: --tx sends a frame in every free slot of 1013 us (radio clock, EM1),
        --rx opens windows of each length of --twins every 200 ms and logs the
        preamble start of each received frame against the window's opening
        (the radio's frame-end timestamp minus 800 us).
turn:   --rx answers each frame of --tx (initiator, one frame every 20 ms)
        --turn-us after its frame-end timestamp, without and with a 100 us
        carrier sense.
drift:  --tx sends about every 100 ms: on its radio clock kept on the crystal
        (EM1); timed by its sleep timer alone (the sleep clock); on its radio
        clock kept across deep sleep by RAIL. --rx receives continuously and
        timestamps the frames; it also reads its own sleep clock against its
        radio clock (fmlog.py drift).
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fmlog  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--parts", default="power,timing,turn,drift")
ap.add_argument("--wins", default="0,100,200,300,600")
ap.add_argument("--twins", default="300,600")
ap.add_argument("--slcp", default="fmmeas_noio.slcp")
ap.add_argument("--conf", default="SL_POWER_MANAGER_INIT_EMU_EM2_DEBUG_ENABLE=0")
ap.add_argument("--defs", default="-DFM_TSYNC=1 -DFM_PMOVH=-12")
ap.add_argument("--chan", type=int, default=5, help="channel of the power images (away from the other kits)")
ap.add_argument("--turn-us", type=int, default=400)
ap.add_argument("--meter", type=int, default=440235509)
ap.add_argument("--rx", type=int, default=440235475)
ap.add_argument("--tx", type=int, default=440240046)
ap.add_argument("--dur", type=float, default=6)
ap.add_argument("--secs", type=float, default=60)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
res = {}


def image(fw, name, defs, slcp="fmmeas.slcp", conf=""):
    env = dict(os.environ, SLCP=slcp, CONF=conf)
    out = subprocess.run([os.path.join(HERE, "build.sh"), os.path.join(a.out, fw), name, defs],
                         capture_output=True, text=True, env=env)
    if out.returncode:
        sys.exit(out.stdout + out.stderr)
    return out.stdout.strip().splitlines()[-1]


def flash(img, sn):
    subprocess.run(["commander", "flash", img, "-s", str(sn), "-d", fmlog.DEV], capture_output=True, check=True)


def console_run(sn, secs, name):
    p = os.path.join(a.out, name + ".txt")
    fmlog.cap(sn, secs, p, reset=True)
    return p


if "power" in a.parts:
    pw = {}
    for w in [int(x) for x in a.wins.split(",")]:
        flash(image("fw_power", f"p{w}", f"-DFM_ROLE=0 -DFM_WIN_US={w} -DFM_CHAN={a.chan} {a.defs}", a.slcp, a.conf), a.meter)
        time.sleep(2)
        p = os.path.join(a.out, f"power_w{w}.csv")
        subprocess.run(["commander", "aem", "dump", "-s", str(a.meter), "--duration", str(a.dur), "-o", p], capture_output=True)
        pw[w] = fmlog.wakes(p)
        print("power window", w, json.dumps(pw[w]), flush=True)
    pts = [(w, r["q_uC"]) for w, r in pw.items() if w > 0 and r["q_uC"]]
    if len(pts) >= 2:
        n = len(pts)
        sx, sy = sum(w for w, _ in pts), sum(q for _, q in pts)
        sxx, sxy = sum(w * w for w, _ in pts), sum(w * q for w, q in pts)
        slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
        res["power"] = {"per_window": pw, "fixed_uC": round((sy - slope * sx) / n, 3), "rx_mA": round(slope * 1000, 3)}
    else:
        res["power"] = {"per_window": pw}
    print(json.dumps({k: v for k, v in res["power"].items() if k != "per_window"}), flush=True)

if "timing" in a.parts:
    tm = {}
    flash(image("fw", "tx1em1", "-DFM_ROLE=2 -DFM_TXPER_US=1013 -DFM_EM1=1"), a.tx)
    for w in [int(x) for x in a.twins.split(",")]:
        flash(image("fw", f"r{w}", f"-DFM_ROLE=1 -DFM_WIN_US={w} {a.defs}"), a.rx)
        tm[w] = fmlog.edge(console_run(a.rx, a.secs, f"timing_w{w}"))
        print("timing window", w, json.dumps(tm[w]), flush=True)
    res["timing"] = tm

if "turn" in a.parts:
    tu = {}
    flash(image("fw", "r6", "-DFM_ROLE=6 -DFM_TXPER_US=20000"), a.tx)
    for cca in (0, 100):
        flash(image("fw", f"r5c{cca}", f"-DFM_ROLE=5 -DFM_TURN_US={a.turn_us} -DFM_CCA_US={cca}"), a.rx)
        subprocess.Popen([sys.executable, os.path.join(HERE, "fmlog.py"), "cap", str(a.tx), "10",
                          os.path.join(a.out, f"turn_cca{cca}_initiator.txt")])
        r = fmlog.turn(console_run(a.rx, 10, f"turn_cca{cca}_responder"))
        time.sleep(1)
        r.update(fmlog.turn(os.path.join(a.out, f"turn_cca{cca}_initiator.txt")))
        tu[cca] = r
        print("turn cca", cca, json.dumps(r), flush=True)
    res["turn"] = tu

if "drift" in a.parts:
    dr = {}
    flash(image("fw", "r4", "-DFM_ROLE=4"), a.rx)
    for name, defs, per in (("crystal", "-DFM_ROLE=2 -DFM_TXPER_US=100000 -DFM_EM1=1", 100000),
                            ("sleep_clock", "-DFM_ROLE=9 -DFM_TICKS=3277", 3277e6 / 32768),
                            ("rail_time_across_sleep", "-DFM_ROLE=2 -DFM_TXPER_US=100000 -DFM_TSYNC=1", 100000)):
        flash(image("fw", "tx_" + name, defs), a.tx)
        dr[name] = fmlog.drift(per, console_run(a.rx, a.secs, "drift_" + name))
        print("drift", name, json.dumps(dr[name]), flush=True)
    res["drift"] = dr

json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1)
