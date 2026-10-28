#!/usr/bin/env python3
"""S1 cold joins: reset the embedded BR, then the LFN; clear the LFN's
credential cache, set traces, join; wait for [Connected]. Consoles must be
running (session.sh start DIR, without rtt: this script handles RTT).

usage: s1_run.py DIR [--reps 10] [--post-s 60] [--lfn-trace all,debug] [--br-trace all,info] [--no-rtt]
"""
import argparse
import json
import os
import re
import subprocess
import time

T = os.path.dirname(os.path.abspath(__file__))
BR, LFN = "440240046", "440235509"
DEV = "EFR32FG25B222F1920IM56"


def send(d, who, cmd):
    with open(os.path.join(d, f"{who}.fifo"), "w") as f:
        f.write(cmd + "\n")


def size(d, who):
    p = os.path.join(d, f"{who}.log")
    return os.path.getsize(p) if os.path.exists(p) else 0


def wait(d, who, pat, start, timeout):
    rx = re.compile(pat)
    t_end = time.time() + timeout
    while time.time() < t_end:
        with open(os.path.join(d, f"{who}.log"), "rb") as f:
            f.seek(start)
            m = rx.search(f.read().decode(errors="replace"))
        if m:
            return m
        time.sleep(0.3)
    return None


RTT = {}


def rtt(sn, log, on):
    pr = RTT.pop(sn, None)
    if pr:
        try:
            os.killpg(pr.pid, 15)
        except ProcessLookupError:
            pass
        pr.wait()
    subprocess.run(["pkill", "-f", f"commander rtt connect --noreset -s {sn}"])
    if on:
        RTT[sn] = subprocess.Popen([os.path.join(T, "rtt.sh"), sn, log], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True)


SILINK = os.environ.get("SILINK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../.deps/slthome/.silabs/slt/installs/archive/commander/silink"))
PTI = {}


def pti(sn, path, on):
    """silink DATA_CHANNEL (PTI) for one kit; it must be closed while commander uses that kit."""
    pr = PTI.pop(sn, None)
    if pr:
        pr.terminate()
        pr.wait()
    subprocess.run([SILINK, "close", "-s", sn, "-c", "DATA_CHANNEL"], capture_output=True, timeout=30)
    for _ in range(20):   # the server releases the adapter a little later
        st = subprocess.run([SILINK, "status"], capture_output=True, text=True, timeout=30).stdout
        if f"{sn} (not connected)" in st:
            break
        time.sleep(0.5)
    time.sleep(1)
    if on:
        out = subprocess.run([SILINK, "open", "-s", sn, "-c", "DATA_CHANNEL"], capture_output=True, text=True,
                             timeout=30).stdout
        m = re.search(r"DATA_CHANNEL: Listening on port (\d+)", out)
        if m:
            PTI[sn] = subprocess.Popen(["python3", os.path.join(T, "dch_cap.py"), m.group(1), path])


def reset(sn, off_s=6.0):
    if sn == LFN:
        # The LFN sleeps in EM2, where its debug port is off: `device reset`
        # cannot reach it. Power-cycle the target instead (a cold boot).
        for attempt in range(10):
            o = subprocess.run(["commander", "adapter", "power", "-s", sn, "off"], capture_output=True, text=True)
            if "ERROR" not in o.stdout + o.stderr:
                break
            time.sleep(2)
        else:
            print("power off failed", flush=True)
        time.sleep(off_s)   # a sleeping LFN draws µA: the board's capacitors hold it up for seconds
        for attempt in range(10):
            o = subprocess.run(["commander", "adapter", "power", "-s", sn, "on"], capture_output=True, text=True)
            if "ERROR" not in o.stdout + o.stderr:
                break
            time.sleep(2)
    else:
        subprocess.run(["commander", "device", "reset", "-s", sn, "-d", DEV], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--post-s", type=int, default=60)
    ap.add_argument("--lfn-trace", default="all,debug")
    ap.add_argument("--br-trace", default="all,info")
    ap.add_argument("--no-rtt", action="store_true")
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--rep0", type=int, default=0)
    a = ap.parse_args()
    d = a.dir
    jf = os.path.join(d, "s1_reps.json")
    res = json.load(open(jf)) if os.path.exists(jf) else []
    res = [r for r in res if r["rep"] < a.rep0]
    for rep in range(a.rep0, a.rep0 + a.reps):
        r = {"rep": rep}
        rtt(BR, None, False)
        rtt(LFN, None, False)
        pos = size(d, "br")
        r["t_br_reset"] = time.time()
        reset(BR)
        if not a.no_rtt:
            rtt(BR, os.path.join(d, "br_rtt.log"), True)
        m = wait(d, "br", r"\[Border router started\]", pos, 90)
        r["t_br_started"] = time.time() if m else None
        send(d, "br", f"wisun set_trace_level {a.br_trace}")
        time.sleep(2)
        pos = size(d, "lfn")
        r["t_lfn_reset"] = time.time()
        for attempt in range(4):
            pti(LFN, None, False)
            p0 = size(d, "lfn")
            reset(LFN, 6.0 + 6.0 * attempt)
            ok = wait(d, "lfn", r"Wi-SUN LFN CLI Application", p0, 10)
            if ok:
                break
            print(f"rep {rep}: LFN reset attempt {attempt} not seen, retrying", flush=True)
        pti(LFN, os.path.join(d, "lfn_dch.bin"), True)
        if not a.no_rtt:
            rtt(LFN, os.path.join(d, "lfn_rtt.log"), True)
        time.sleep(4)
        send(d, "lfn", "wisun clear_credential_cache")
        time.sleep(1.5)
        send(d, "lfn", f"wisun set_trace_level {a.lfn_trace}")
        time.sleep(1.5)
        pos = size(d, "lfn")
        r["log_offset_lfn"] = pos
        r["t_join"] = time.time()
        send(d, "lfn", "wisun join_fan11")
        m = wait(d, "lfn", r"\[Connected: (\d+) s\]|\[Failed: not disconnected\]", pos, a.timeout)
        if m and m.group(0).startswith("[Failed"):
            print(f"rep {rep}: LFN was not reset", flush=True)
            m = None
        r["t_connected"] = time.time() if m else None
        r["connected_stack_s"] = int(m.group(1)) if m else None
        print(f"rep {rep}: connected after {r['t_connected'] - r['t_join']:.1f} s" if m else f"rep {rep}: no join",
              flush=True)
        time.sleep(a.post_s)
        res.append(r)
        with open(jf, "w") as f:
            json.dump(res, f, indent=1)
    rtt(BR, None, False)
    rtt(LFN, None, False)
    pti(LFN, None, False)


if __name__ == "__main__":
    main()
