#!/usr/bin/env python3
"""Start/stop wsbrd (+ wisun_rcp on kit B) for the LFN black-box test, and the LFN console.

usage: wsbrd.py start DIR [--extra FILE] [--no-lfn-console]
       wsbrd.py restart DIR [--extra FILE]     (wsbrd only; the LFN console keeps running)
       wsbrd.py stop DIR
wsbrd's log is appended to DIR/wsbrd.log; each start writes a new pcap DIR/wsbrd_<n>.pcapng
(frames as wsbrd sees them, unencrypted). DIR/br.log gets one line per start/stop so that
timeline.py can place them. Config: wsbrd_p1.conf.in (+ lines of --extra) -> DIR/wsbrd.conf.
wsbrd needs cap_net_admin,cap_net_raw,cap_net_bind_service (setcap) on its binary.
The LFN console (kit A, 9600 baud, RTS/CTS) logs to DIR/lfn.log, commands via DIR/lfn.fifo.
"""
import argparse
import glob
import re
import os
import signal
import subprocess
import sys
import time

T = os.path.dirname(os.path.abspath(__file__))
DEPS = os.path.abspath(os.environ.get("DEPS", os.path.join(T, "../../../../.deps")))
WSBRD = os.environ.get("WSBRD", os.path.join(DEPS, "wisun-br-linux-v2.10.3/build/wsbrd"))
CON = os.path.join(T, "../console.py")
LFN_SN = "440235509"


def note(d, txt):
    with open(os.path.join(d, "br.log"), "a") as f:
        f.write(f"{time.time():.3f} {txt}\n")


def pidfile(d, who):
    return os.path.join(d, f"{who}.pid")


def kill(d, who):
    p = pidfile(d, who)
    if not os.path.exists(p):
        return
    pid = int(open(p).read())
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            os.kill(pid, 0)
            time.sleep(0.1)
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.remove(p)


def start_wsbrd(d, extra=None, fresh_storage=True, wait_ready=60):
    """Start wsbrd; return (pcap path, time of 'ready' or None)."""
    d = os.path.abspath(d)
    os.makedirs(os.path.join(d, "storage"), exist_ok=True)
    if fresh_storage:
        for f in glob.glob(os.path.join(d, "storage", "*")):
            os.remove(f)
    n = len(glob.glob(os.path.join(d, "wsbrd_*.pcapng")))
    pcap = os.path.join(d, f"wsbrd_{n:02d}.pcapng")
    conf = open(os.path.join(T, "wsbrd_p1.conf.in")).read()
    conf = conf.replace("@DEPS@", DEPS).replace("@RUN@", d).replace("@PCAP@", pcap)
    if extra:
        conf += "\n" + open(extra).read()
    if not fresh_storage:
        # wsbrd refuses fixed (l)gtk[] values when it reloads its keys from storage
        conf = "\n".join(l for l in conf.splitlines() if not re.match(r"\s*l?gtk\[", l)) + "\n"
    open(os.path.join(d, "wsbrd.conf"), "w").write(conf)
    log = os.path.join(d, "wsbrd.log")
    pos = os.path.getsize(log) if os.path.exists(log) else 0
    lf = open(log, "ab")
    lf.write(b"%.3f ==== wsbrd start, pcap %s\n" % (time.time(), pcap.encode()))
    lf.flush()
    pr = subprocess.Popen([WSBRD, "-F", os.path.join(d, "wsbrd.conf")], stdout=lf, stderr=subprocess.STDOUT,
                          start_new_session=True)
    open(pidfile(d, "wsbrd"), "w").write(str(pr.pid))
    note(d, f"wsbrd started pid {pr.pid} pcap {os.path.basename(pcap)}")
    t_end = time.time() + wait_ready
    while time.time() < t_end:
        with open(log, "rb") as f:
            f.seek(pos)
            txt = f.read()
        if b"Border Router is ready" in txt:
            note(d, "[Border router started]")
            return pcap, time.time()
        if pr.poll() is not None:
            break
        time.sleep(0.2)
    return pcap, None


def stop_wsbrd(d):
    kill(d, "wsbrd")
    note(d, "wsbrd stopped")


def start_lfn_console(d):
    tty = os.path.realpath(glob.glob(f"/dev/serial/by-id/*000{LFN_SN}-if00")[0])
    env = dict(os.environ, CONSOLE_RTSCTS="1", CONSOLE_BAUD="9600")
    pr = subprocess.Popen([sys.executable, CON, tty, os.path.join(d, "lfn.log"), os.path.join(d, "lfn.fifo")],
                          env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    open(pidfile(d, "lfncon"), "w").write(str(pr.pid))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["start", "restart", "stop"])
    ap.add_argument("dir")
    ap.add_argument("--extra")
    ap.add_argument("--keep-storage", action="store_true")
    ap.add_argument("--no-lfn-console", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    if a.cmd in ("restart", "stop"):
        stop_wsbrd(a.dir)
    if a.cmd == "stop":
        kill(a.dir, "lfncon")
        return
    if a.cmd == "start" and not a.no_lfn_console:
        start_lfn_console(a.dir)
    pcap, t = start_wsbrd(a.dir, a.extra, not a.keep_storage)
    print(f"pcap {pcap}, ready {t}")
    if t is None:
        sys.exit(1)


if __name__ == "__main__":
    main()
