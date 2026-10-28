#!/usr/bin/env python3
"""HW5: AEM current and firmware power statistics over the same windows.

Both kits run images built with -DFW_POWER_STATS=ON. The measured kit is the
one whose AEM is read. The script cold-boots the network (BR first), waits for
the RN to join, settles, lowers the trace level, then runs N windows per mode:

  idle  joined, no application traffic
  ping  the peer pings the measured node once per second for the whole window

Per window: "pstats clear" on the measured node, `commander aem measure` for
the window length, then "pstats"; the console logs keep everything.

usage: tests/hw/hw5_power.py --meas-role rn|br --meas-sn SN --meas-tty TTY \
           --peer-sn SN --peer-tty TTY --out DIR [--windows 10] [--window-s 30]
           [--modes idle,ping] [--no-boot]
Then: tests/hw/pstats_parse.py DIR --elf build/fw-pstats/wisun_<role>.elf
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = "EFR32FG25B222F1920IM56"


class Console:
    def __init__(self, name, tty, outdir):
        self.log = os.path.join(outdir, f"{name}.log")
        self.fifo = os.path.join(outdir, f"{name}.fifo")
        if os.path.exists(self.fifo):
            os.unlink(self.fifo)
        os.mkfifo(self.fifo)
        self.proc = subprocess.Popen([sys.executable, os.path.join(HERE, "console.py"), tty, self.log, self.fifo])
        time.sleep(0.5)
        self.fd = os.open(self.fifo, os.O_WRONLY)

    def send(self, cmd):
        os.write(self.fd, (cmd + "\n").encode())

    def size(self):
        return os.path.getsize(self.log) if os.path.exists(self.log) else 0

    def text(self, start=0):
        with open(self.log, "rb") as f:
            f.seek(start)
            return f.read().decode(errors="replace")

    def wait_for(self, pattern, start, timeout):
        t_end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < t_end:
            m = rx.search(self.text(start))
            if m:
                return m
            time.sleep(0.5)
        return None

    def close(self):
        os.close(self.fd)
        self.proc.terminate()


def commander(*args):
    return subprocess.run(["commander", *args], capture_output=True, text=True).stdout


def gua(con):
    pos = con.size()
    con.send("status")
    m = con.wait_for(r"ws0 addr (fd[0-9a-f:]+)", pos, 10)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meas-role", choices=["rn", "br"], required=True)
    ap.add_argument("--meas-sn", required=True)
    ap.add_argument("--meas-tty", required=True)
    ap.add_argument("--peer-sn", required=True)
    ap.add_argument("--peer-tty", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--windows", type=int, default=10)
    ap.add_argument("--window-s", type=int, default=30)
    ap.add_argument("--modes", default="idle,ping")
    ap.add_argument("--settle-s", type=int, default=60)
    ap.add_argument("--trace", default="0")
    ap.add_argument("--no-boot", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    role = {"meas": a.meas_role, "peer": "br" if a.meas_role == "rn" else "rn"}
    cons = {"meas": Console("meas_" + role["meas"], a.meas_tty, a.out),
            "peer": Console("peer_" + role["peer"], a.peer_tty, a.out)}
    sn = {"meas": a.meas_sn, "peer": a.peer_sn}
    rn = "meas" if role["meas"] == "rn" else "peer"
    br = "peer" if rn == "meas" else "meas"
    meta = {"meas_role": role["meas"], "meas_sn": a.meas_sn, "peer_sn": a.peer_sn, "windows": []}
    try:
        if not a.no_boot:
            commander("device", "reset", "-s", sn[br], "-d", DEV)
            time.sleep(3)
            pos = cons[rn].size()
            commander("device", "reset", "-s", sn[rn], "-d", DEV)
            t0 = time.time()
            m = None
            while time.time() - t0 < 600 and not m:
                cons[rn].send("status")
                m = cons[rn].wait_for(r"== RN \S+ up \d+s ready=1", pos, 15)
            if not m:
                print("RN did not join in 600 s", file=sys.stderr)
                return 1
            meta["join_s"] = round(time.time() - t0, 1)
            print(f"RN joined after {meta['join_s']} s (host time)", flush=True)
            time.sleep(a.settle_s)
        for k in cons:
            cons[k].send(f"trace {a.trace}")
        time.sleep(2)
        meas_addr, peer_addr = gua(cons["meas"]), gua(cons["peer"])
        meta.update(meas_addr=meas_addr, peer_addr=peer_addr)
        print(f"measured {role['meas']} {meas_addr}, peer {peer_addr}", flush=True)
        for mode in a.modes.split(","):
            for i in range(a.windows):
                if mode == "ping":
                    cons["peer"].send(f"ping {meas_addr} {a.window_s + 2} 16")
                    time.sleep(0.5)
                pos = cons["meas"].size()
                cons["meas"].send("pstats clear")
                t_start = time.time()
                out = commander("aem", "measure", "-s", sn["meas"], "--windowlength", str(a.window_s * 1000))
                t_stop = time.time()
                cons["meas"].send("pstats")
                cons["meas"].wait_for(r"ps end", pos, 10)
                cur = re.search(r"Current \[mA\]: ([0-9.eE+-]+)", out)
                volt = re.search(r"Voltage \[V\] : ([0-9.]+)", out)
                w = {"mode": mode, "i": i, "log_offset": pos, "t_start": t_start, "t_stop": t_stop,
                     "aem_ma": float(cur.group(1)) if cur else None, "aem_v": float(volt.group(1)) if volt else None}
                meta["windows"].append(w)
                print(f"{mode} {i}: AEM {w['aem_ma']} mA", flush=True)
                if mode == "ping":
                    time.sleep(4)
        # One AEM trace for the shape of the wakes.
        subprocess.run(["commander", "aem", "dump", "-s", sn["meas"], "--duration", "10",
                        "-o", os.path.join(a.out, "aem_dump_idle_10s.csv")], capture_output=True)
    finally:
        with open(os.path.join(a.out, "meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        for c in cons.values():
            c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
