#!/usr/bin/env python3
"""Start/stop the packet-trace (PTI) capture of one kit through Commander's silink.

usage: pti.py start SN OUT.bin     (silink DATA_CHANNEL + dch_cap.py, detached; pid in OUT.bin.pid)
       pti.py stop SN OUT.bin
silink holds the J-Link: stop it before `commander` uses the same kit (flash, reset, power, AEM).
"""
import os
import re
import signal
import subprocess
import sys
import time

T = os.path.dirname(os.path.abspath(__file__))
SILINK = os.environ.get("SILINK", os.path.join(T, "../../../../.deps/slthome/.silabs/slt/installs/archive/commander/silink"))


def close(sn):
    subprocess.run([SILINK, "close", "-s", sn, "-c", "DATA_CHANNEL"], capture_output=True, timeout=30)
    for _ in range(20):   # the server releases the adapter a little later
        st = subprocess.run([SILINK, "status"], capture_output=True, text=True, timeout=30).stdout
        if f"{sn} (not connected)" in st or sn not in st:
            break
        time.sleep(0.5)
    time.sleep(1)


def stop(sn, out):
    pf = out + ".pid"
    if os.path.exists(pf):
        try:
            os.kill(int(open(pf).read()), signal.SIGTERM)
        except ProcessLookupError:
            pass
        os.remove(pf)
    close(sn)


def start(sn, out):
    stop(sn, out)
    o = subprocess.run([SILINK, "open", "-s", sn, "-c", "DATA_CHANNEL"], capture_output=True, text=True, timeout=30).stdout
    m = re.search(r"DATA_CHANNEL: Listening on port (\d+)", o)
    if not m:
        sys.exit("silink open failed: " + o)
    pr = subprocess.Popen([sys.executable, os.path.join(T, "dch_cap.py"), m.group(1), out], start_new_session=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    open(out + ".pid", "w").write(str(pr.pid))


if __name__ == "__main__":
    {"start": start, "stop": stop}[sys.argv[1]](sys.argv[2], sys.argv[3])
