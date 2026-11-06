#!/usr/bin/env python3
"""Helpers for the LFN black-box test, phases 2-3 (our firmware against Silicon Labs' stack).

Kits are named by J-Link serial number; their consoles are found through
/dev/serial/by-id (the ttyACM numbers change between enumerations). Every console runs
tests/hw/console.py in its own session, with a pid file in the run directory, so that
only the processes started here are stopped.
"""
import glob
import os
import re
import signal
import subprocess
import sys
import time

T = os.path.dirname(os.path.abspath(__file__))
DEPS = os.path.abspath(os.environ.get("DEPS", os.path.join(T, "../../../../.deps")))
CON = os.path.join(T, "../console.py")
DEV = "EFR32FG25B222F1920IM56"
A, B = "440235509", "440240046"      # A: working AEM (the LFN); B: the parent
FW = os.path.abspath(os.path.join(T, "../../.."))


def tty(sn):
    return os.path.realpath(glob.glob(f"/dev/serial/by-id/*000{sn}-if00")[0])


def commander(*args, check=False):
    r = subprocess.run(["commander", *args], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"commander {' '.join(args)}: {r.stdout}{r.stderr}")
    return r.stdout + r.stderr


def flash(sn, image):
    out = commander("flash", image, "-s", sn, "-d", DEV)
    ok = "DONE" in out or "successfully" in out.lower()
    print(f"flash {sn} {os.path.basename(image)}: {'ok' if ok else out[-300:]}", flush=True)
    return ok


def vcom(sn, baud, rtscts):
    commander("vcom", "config", "-s", sn, "--baudrate", str(baud), "--handshake", "rtscts" if rtscts else "none")


def reset(sn):
    commander("device", "reset", "-s", sn, "-d", DEV)


def power_cycle(sn, off_s=6.0):
    """A sleeping LFN (EM2) cannot be reset over SWD: switch the target supply off and on."""
    commander("adapter", "power", "-s", sn, "off")
    time.sleep(off_s)
    commander("adapter", "power", "-s", sn, "on")


def pidfile(d, who):
    return os.path.join(d, f"{who}.pid")


def stop(d, who):
    p = pidfile(d, who)
    if not os.path.exists(p):
        return
    pid = int(open(p).read())
    try:
        os.killpg(pid, signal.SIGTERM)
        for _ in range(30):
            os.kill(pid, 0)
            time.sleep(0.1)
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.remove(p)


def console(d, who, sn, baud=115200, rtscts=False):
    """Start logging kit sn to d/<who>.log, commands through d/<who>.fifo."""
    stop(d, who)
    env = dict(os.environ, CONSOLE_BAUD=str(baud), CONSOLE_RTSCTS="1" if rtscts else "0",
               CONSOLE_APPEND="1")   # searches use offsets; a restarted console keeps the run's log
    pr = subprocess.Popen([sys.executable, CON, tty(sn), os.path.join(d, f"{who}.log"), os.path.join(d, f"{who}.fifo")],
                          env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    open(pidfile(d, who), "w").write(str(pr.pid))
    time.sleep(0.5)


def send(d, who, cmd):
    with open(os.path.join(d, f"{who}.fifo"), "w") as f:
        f.write(cmd + "\n")


def size(d, who):
    p = os.path.join(d, f"{who}.log")
    return os.path.getsize(p) if os.path.exists(p) else 0


def wait(d, who, pat, start, timeout):
    """Wait for pat (regex, bytes-decoded) in d/<who>.log after offset start; return (match, line time) or None."""
    rx = re.compile(pat)
    t_end = time.time() + timeout
    p = os.path.join(d, f"{who}.log")
    while time.time() < t_end:
        if os.path.exists(p):
            with open(p, "rb") as f:
                f.seek(start)
                for line in f.read().decode(errors="replace").splitlines():
                    m = rx.search(line)
                    if m:
                        try:
                            return m, float(line.split(" ", 1)[0])
                        except ValueError:
                            return m, time.time()
        time.sleep(0.3)
    return None


def text(d, who, start=0):
    p = os.path.join(d, f"{who}.log")
    with open(p, "rb") as f:
        f.seek(start)
        return f.read().decode(errors="replace")


def aem_measure(sn, seconds):
    out = commander("aem", "measure", "-s", sn, "--windowlength", str(int(seconds * 1000)))
    m = re.search(r"Current \[(m|u)A\]:\s*([-0-9.eE]+)", out)
    if not m:
        return None, out
    v = float(m.group(2))
    return (v * 1000 if m.group(1) == "m" else v), out
