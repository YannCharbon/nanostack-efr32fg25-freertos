#!/usr/bin/env python3
"""Measurements of the fire alarm mesh stack's radio on the FG25 kits, with
the measurement firmware of this folder (build.sh). Results as JSON in OUT.

usage: run.py OUT [--parts power,timing] [--wins 0,100,200,300,400,600]
              [--meter 440235509] [--rx 440235475] [--tx 440240046] [--dur 6] [--secs 90]

power:  on the kit with the energy monitor (--meter), one image per window
        length; the floor is the image without radio (window 0); the charge
        per listening moment is (mean - floor) x period, and a line fitted
        against the window length gives the fixed cost and the RX current.
timing: --tx sends a frame every 1013 us (back to back), --rx opens windows
        of each length of --twins every 200 ms and logs the instant of each
        received frame's preamble start against the window's start; the
        reception probability per 25 us of that offset is the count over
        (windows x 25 / 1013).
"""
import argparse, json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "radio_power"))
from power import dump, summary  # noqa: E402
import serial  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--parts", default="power,timing")
ap.add_argument("--wins", default="0,100,200,300,400,600")
ap.add_argument("--twins", default="300,600")
ap.add_argument("--meter", type=int, default=440235509)
ap.add_argument("--rx", type=int, default=440235475)
ap.add_argument("--tx", type=int, default=440240046)
ap.add_argument("--dur", type=float, default=6)
ap.add_argument("--secs", type=float, default=90)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
FW = os.path.join(a.out, "fw")
DEV = "EFR32FG25B222F1920IM56"
res = {}


def image(name, defs):
    out = subprocess.run([os.path.join(HERE, "build.sh"), FW, name, defs], capture_output=True, text=True)
    if out.returncode:
        sys.exit(out.stdout + out.stderr)
    return out.stdout.strip().splitlines()[-1]


def flash(img, sn):
    subprocess.run(["commander", "flash", img, "-s", str(sn), "-d", DEV], capture_output=True, check=True)


if "power" in a.parts:
    pw = {}
    for w in [int(x) for x in a.wins.split(",")]:
        flash(image(f"p{w}", f"-DFM_ROLE=0 -DFM_WIN_US={w}"), a.meter)
        time.sleep(1.5)
        p = os.path.join(a.out, f"power_w{w}.csv")
        dump(a.meter, a.dur, p)
        pw[w] = summary(p)["mean_ma"]
        print("power window", w, "us: mean", round(pw[w] * 1000, 2), "uA", flush=True)
    floor = pw.get(0)
    if floor is not None:
        pts = [(w, (m - floor) * 200.0) for w, m in pw.items() if w > 0]   # uC per window (mA x 200 ms)
        n = len(pts); sx = sum(w for w, _ in pts); sy = sum(q for _, q in pts)
        sxx = sum(w * w for w, _ in pts); sxy = sum(w * q for w, q in pts)
        slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
        res["power"] = {"mean_ma": pw, "floor_ua": floor * 1000, "uc_per_window": dict(pts),
                        "fixed_uc": (sy - slope * sx) / n, "rx_ma": slope * 1000}
        print(json.dumps(res["power"]), flush=True)

if "timing" in a.parts:
    tm = {}
    flash(image("tx", "-DFM_ROLE=2 -DFM_TXPER_US=1013"), a.tx)
    for w in [int(x) for x in a.twins.split(",")]:
        flash(image(f"r{w}", f"-DFM_ROLE=1 -DFM_WIN_US={w}"), a.rx)
        s = serial.Serial(f"/dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_000{a.rx}-if00", 115200, timeout=0.5)
        subprocess.run(["commander", "device", "reset", "-s", str(a.rx), "-d", DEV], capture_output=True)
        t_end = time.time() + a.secs
        wins, offs = 0, []
        while time.time() < t_end:
            line = s.readline().decode(errors="replace").split()
            if len(line) == 2 and line[0] == "W": wins += 1
            elif len(line) == 3 and line[0] == "R": offs.append(int(line[2]))
        s.close()
        bins = {}
        for o in offs: bins[25 * (o // 25)] = bins.get(25 * (o // 25), 0) + 1
        exp = wins * 25 / 1013.0
        prob = {b: round(c / exp, 2) for b, c in sorted(bins.items())} if wins else {}
        tm[w] = {"windows": wins, "received": len(offs), "prob_by_offset_us": prob,
                 "offset_min": min(offs) if offs else None, "offset_max": max(offs) if offs else None}
        print("timing window", w, json.dumps(tm[w]), flush=True)
    res["timing"] = tm

json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1)
