#!/usr/bin/env python3
"""LFN black-box test, phases 2-3: scenarios on the kits (root repo:
documents/pre-study/investigation/LFN_BlackBox_Phase2_3_Results.md).

Phase 2, our LFN (wisun_lfn, kit A) under a Silicon Labs parent on kit B:
  p2w   wsbrd + wisun_rcp (UART variant) on kit B
  p2e   the embedded border router wisun_soc_brcli on kit B
Phase 3, Silicon Labs' LFN (wisun_soc_lfn_cli, kit A) under our border router (wisun_br, kit B):
  p3

usage: p23_run.py {p2w,p2e,p3} DIR [--reps N] [--pings N] [--aem N] [--restart] [--rotate] [--mc]
Every rep is a cold join (parent restarted with a new PAN for p2w), followed by --pings
pings each way (one at a time, 6-9 s apart), the multicast pings (--mc), --aem idle
windows of 60 s on kit A's AEM (after the last rep), a parent restart (--restart) and an
LGTK rotation (--rotate). Results: DIR/results.json; consoles DIR/{lfn,br}.log; wsbrd's log
and pcaps (p2w). The flashing is done beforehand (p23lib.flash).
"""
import argparse
import json
import os
import random
import re
import subprocess
import time

import p23lib as L
import wsbrd as W

ADDR_LFN = {"p2w": r"(fd00:5eed:[0-9a-f:]+)", "p2e": r"(f[cd][0-9a-f]{2}:[0-9a-f:]+)", "p3": r"(fd12:3456:[0-9a-f:]+)"}


# fd12:3456::/64 + the kit's EUI-64 interface ID (phase 1: kit A b4:35:22:ff:fe:98:2e:a6, kit B ...:23:91)
EUI_ADDR = {L.A: "fd12:3456::b635:22ff:fe98:2ea6", L.B: "fd12:3456::b635:22ff:fe98:2391"}


def host_ping(addr, timeout=15):
    t0 = time.time()
    r = subprocess.run(["ping", "-6", "-c", "1", "-W", str(timeout), "-I", "tunlfn", addr], capture_output=True, text=True)
    return (time.time() - t0) if r.returncode == 0 else None


def tun_addr():
    out = subprocess.run(["ip", "-6", "addr", "show", "tunlfn"], capture_output=True, text=True).stdout
    m = re.search(r"inet6 (fd00:5eed:[0-9a-f:]+)/", out)
    return m.group(1) if m else None


class Run:
    def __init__(self, a):
        self.a = a
        self.d = os.path.abspath(a.dir)
        os.makedirs(self.d, exist_ok=True)
        self.res = {"mode": a.mode, "reps": [], "pings": [], "mc": [], "aem_ua": [], "events": []}

    def save(self):
        json.dump(self.res, open(os.path.join(self.d, "results.json"), "w"), indent=1)

    def ev(self, txt):
        print(f"{time.strftime('%H:%M:%S')} {txt}", flush=True)
        self.res["events"].append((time.time(), txt))
        self.save()

    # ---------------------------------------------------------------- parents
    def parent_start(self, fresh=True):
        m = self.a.mode
        if m == "p2w":
            W.stop_wsbrd(self.d)
            pcap, t = W.start_wsbrd(self.d, self.a.extra, fresh_storage=fresh)
            self.ev(f"wsbrd started ({os.path.basename(pcap)}, ready {t is not None})")
            return t
        p = L.size(self.d, "br")
        L.reset(L.B)
        pat = r"Border Router started|BR started|network started|Wi-SUN [Bb]order [Rr]outer" if m == "p2e" else r"BR: interfaces up"
        r = L.wait(self.d, "br", pat, p, 40)
        self.ev(f"parent reset, banner {'seen' if r else 'NOT seen'}")
        return r[1] if r else time.time()

    def parent_stop(self):
        if self.a.mode == "p2w":
            W.stop_wsbrd(self.d)
        else:
            L.commander("adapter", "power", "-s", L.B, "off")   # restarted by parent_start (power on + reset)

    # -------------------------------------------------------------------- LFN
    def lfn_join(self, timeout):
        m = self.a.mode
        p = L.size(self.d, "lfn")
        t_reset = time.time()
        if m == "p3":
            for attempt in range(3):
                L.power_cycle(L.A, 6.0 + 4 * attempt)
                if L.wait(self.d, "lfn", r"Wi-SUN LFN CLI Application", p, 12):
                    break
            time.sleep(2)
            L.send(self.d, "lfn", "wisun clear_credential_cache")
            time.sleep(1.5)
            p = L.size(self.d, "lfn")
            t_reset = time.time()
            L.send(self.d, "lfn", "wisun join_fan11")
            r = L.wait(self.d, "lfn", r"\[Connected: \d+ s\]|\[Failed[^\]]*\]", p, timeout)
            ok = r and r[0].group(0).startswith("[Connected")
        else:
            L.reset(L.A)
            if not L.wait(self.d, "lfn", r"interfaces up", p, 10):
                L.power_cycle(L.A, 3.0)
                t_reset = time.time()
            r = L.wait(self.d, "lfn", r"ARM_NWK_BOOTSTRAP_READY", p, timeout)
            ok = bool(r)
        return (r[1] - t_reset) if ok else None, p

    def lfn_addr(self, start=0):
        txt = L.text(self.d, "lfn", start)
        if self.a.mode == "p3":
            # our border router's DHCPv6 server gives prefix + the EUI-64 interface ID
            return EUI_ADDR[L.A]
        ms = re.findall(r"(?:ws0 addr|DHCPv6) (" + ADDR_LFN[self.a.mode][1:-1] + ")", txt)
        return ms[-1] if ms else None

    def br_addr(self):
        if self.a.mode == "p2w":
            return tun_addr()
        txt = L.text(self.d, "br")
        ms = re.findall(r"(?:ws0 addr |IPv6 address[^\n]*?)(fd12:3456:[0-9a-f:]+)", txt)
        return ms[-1] if ms else EUI_ADDR[L.B]

    # ------------------------------------------------------------------ pings
    def ping_down(self, addr):
        """Parent -> LFN, one echo request; round trip in s or None."""
        if self.a.mode == "p2w":
            return host_ping(addr)
        p = L.size(self.d, "br")
        t0 = time.time()
        if self.a.mode == "p2e":
            L.send(self.d, "br", f"wisun ping {addr}")
            r = L.wait(self.d, "br", r"bytes from \S+ icmp_seq=\d+ time=(\d+) ms|bytes from \S+: icmp_seq=\d+ time=(\d+) ms|[Tt]imeout|Failed", p, 25)
            if r and (r[0].group(1) or r[0].group(2)):
                return int(r[0].group(1) or r[0].group(2)) / 1000
            ok = False
        else:
            L.send(self.d, "br", f"ping {addr} 1 32 1000")
            r = L.wait(self.d, "br", r"(\d+) received", p, 25)
            ok = r and int(r[0].group(1)) > 0
            if ok:
                rr = L.wait(self.d, "br", r"from [0-9a-f:]+.*?time[=:] ?([0-9.]+) ?ms|rtt[^\n]*?([0-9.]+) ms", p, 1)
                if rr:
                    return float(rr[0].group(1) or rr[0].group(2)) / 1000
        return (r[1] - t0) if ok else None

    def ping_up(self, addr):
        """LFN -> parent, one echo request."""
        p = L.size(self.d, "lfn")
        t0 = time.time()
        if self.a.mode == "p3":
            L.send(self.d, "lfn", f"wisun ping {addr}")
            r = L.wait(self.d, "lfn", r"Ping statistics[^\n]*|[Rr]eply|bytes from|\[Failed|time-?out|[Ee]rror", p, 25)
            ok = r and not re.search(r"Failed|time-?out|[Ee]rror", r[0].group(0))
        else:
            L.send(self.d, "lfn", f"ping {addr} 1 32 1000")
            r = L.wait(self.d, "lfn", r"(\d+) received", p, 25)
            ok = r and int(r[0].group(1)) > 0
        return (r[1] - t0) if ok else None

    def pings(self, n):
        la, ba = self.lfn_addr(), self.br_addr()
        self.ev(f"pings: LFN {la}, parent {ba}")
        if not la or not ba:
            return
        p_lfn = L.size(self.d, "lfn")
        t0 = time.time()
        for i in range(n):
            ways = (("down", self.ping_down, la), ("up", self.ping_up, ba)) if self.a.mode == "p3" else (("down", self.ping_down, la),)
            for way, f, ad in ways:
                rtt = f(ad)
                self.res["pings"].append({"way": way, "i": i, "rtt_s": rtt, "t": time.time()})
                print(f"  ping {way} {i}: {rtt}", flush=True)
                time.sleep(random.uniform(6, 9))
        if self.a.mode != "p3":
            self.lfn_pings(p_lfn, t0)
        self.save()

    def lfn_pings(self, start, t0):
        """Our LFN's own periodic pings to its parent (APP_LFN_PING_PERIOD_S: its console cannot
        receive in EM2): one result line per ping in its log."""
        txt = L.text(self.d, "lfn", start)
        for m in re.finditer(r"^(\d+\.\d+) --- (\S+) ping statistics: (\d+) transmitted, (\d+) received(?:[^\n=]*= (\d+)/)?", txt, re.M):
            rtt = int(m.group(5)) / 1000 if m.group(5) else None
            ok = int(m.group(4)) > 0
            self.res["pings"].append({"way": "up", "rtt_s": rtt if ok else None, "t": float(m.group(1)), "dst": m.group(2)})
            print(f"  LFN ping up at {float(m.group(1)) - t0:.0f} s: {rtt if ok else None}", flush=True)

    def multicast(self, groups=("ff02::1", "ff03::1")):
        """Multicast echo requests from the parent; did the LFN answer?"""
        la = self.lfn_addr()
        for g in groups:
            for i in range(3):
                ok = None
                if self.a.mode == "p2w":
                    # wsbrd copies ff02::1 to an LFN as a MAC unicast (registered groups too); our
                    # LFN answers the copy's source, wsbrd itself: count its echo replies (pcap)
                    t_send = time.time()
                    subprocess.run(["ping", "-6", "-c", "1", "-w", "15", "-I", "tunlfn", g], capture_output=True, text=True)
                    ok = self.lfn_echo_replies(t_send, time.time()) > 0
                else:
                    p = L.size(self.d, "br")
                    L.send(self.d, "br", f"ping {g} 1 32 1000" if self.a.mode == "p3" else f"wisun ping {g}")
                    time.sleep(16)
                    ok = la is not None and la.split("::")[-1] in L.text(self.d, "br", p)
                self.res["mc"].append({"group": g, "i": i, "lfn_replied": ok})
                print(f"  multicast {g} {i}: LFN replied {ok}", flush=True)
                time.sleep(random.uniform(3, 6))
        self.save()

    def lfn_echo_replies(self, t0, t1):
        pcaps = sorted(f for f in os.listdir(self.d) if f.startswith("wsbrd_") and f.endswith(".pcapng"))
        if not pcaps:
            return 0
        out = subprocess.run([W.TSHARK if hasattr(W, "TSHARK") else os.path.join(L.DEPS, "tshark/tshark.sh"), "-r",
                              os.path.join(self.d, pcaps[-1]), "-T", "fields", "-e", "frame.time_epoch", "-e", "wpan.src64",
                              "-e", "icmpv6.type"], capture_output=True, text=True).stdout
        n = 0
        for l in out.splitlines():
            f = l.split("\t")
            if len(f) == 3 and f[2] == "129" and f[1].endswith("2e:a6") and t0 <= float(f[0]) <= t1 + 1:
                n += 1
        return n

    def aem(self, n):
        # silink must not hold kit A; the LFN's console stays open
        for i in range(n):
            ua, out = L.aem_measure(L.A, 60)
            self.res["aem_ua"].append(ua)
            print(f"  AEM window {i}: {ua} uA", flush=True)
            self.save()

    # -------------------------------------------------------------- scenarios
    def main(self):
        a = self.a
        m = a.mode
        if m != "p2w":
            L.console(self.d, "br", L.B, 115200, rtscts=(m == "p2e"))
        L.console(self.d, "lfn", L.A, 9600 if m == "p3" else 115200, rtscts=(m == "p3"))
        for rep in range(a.reps):
            self.parent_start(fresh=True)
            time.sleep(a.parent_wait)
            dt, pos = self.lfn_join(a.timeout)
            self.res["reps"].append({"rep": rep, "join_s": dt, "log_offset": pos})
            self.ev(f"rep {rep}: LFN joined after {dt}" if dt else f"rep {rep}: LFN did not join")
        if a.pings:
            time.sleep(35)        # our LFN's traces go quiet 30 s after its join
            self.pings(a.pings)
        if a.mc:
            self.multicast()
        if a.aem:
            time.sleep(10)
            self.aem(a.aem)
        if a.rotate:
            self.rotate()
        if a.restart:
            self.restart()
        self.save()

    def rotate(self):
        if self.a.mode == "p3":
            L.send(self.d, "br", "lgtk rotate")
            self.ev("LGTK rotation requested on our border router")
        else:
            self.ev("LGTK rotation: wsbrd's timers (--extra s6_lgtk_fast.conf)")
        t_end = time.time() + self.a.rotate_wait
        la, ba = self.lfn_addr(), self.br_addr()
        p_lfn, t0 = L.size(self.d, "lfn"), time.time()
        ways = (("down", self.ping_down, la), ("up", self.ping_up, ba)) if self.a.mode == "p3" else (("down", self.ping_down, la),)
        while time.time() < t_end:
            for way, f, ad in ways:
                rtt = f(ad)
                self.res["pings"].append({"way": way, "phase": "rotate", "rtt_s": rtt, "t": time.time()})
                print(f"  rotate ping {way}: {rtt}", flush=True)
                time.sleep(random.uniform(20, 30))
        if self.a.mode != "p3":
            self.lfn_pings(p_lfn, t0)
        self.save()

    def restart(self):
        """Parent restart: the LFN must declare it lost (1.5 x maintain_parent_time sync periods) and rejoin."""
        p = L.size(self.d, "lfn")
        if self.a.mode == "p2w":
            self.parent_stop()
            self.ev("parent stopped")
            time.sleep(self.a.restart_off)
            self.parent_start(fresh=False)
        else:
            # kit B's target supply bypasses the AEM switch (phase 1 §1.1): a reset
            self.parent_start()
        pat = r"Connected: \d+ s|\[Connected" if self.a.mode == "p3" else r"ARM_NWK_BOOTSTRAP_READY"
        lost = L.wait(self.d, "lfn", r"parent .* lost|Connection lost|[Dd]isconnected|Join state 1: Select PAN", p, 900)
        self.ev(f"LFN lost its parent: {lost[1] if lost else None}")
        back = L.wait(self.d, "lfn", pat, p, 900) if lost else None
        self.ev(f"LFN joined again: {back[1] if back else None}")
        self.res["restart"] = {"t_lost": lost[1] if lost else None, "t_back": back[1] if back else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["p2w", "p2e", "p3"])
    ap.add_argument("dir")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--pings", type=int, default=0)
    ap.add_argument("--mc", action="store_true")
    ap.add_argument("--aem", type=int, default=0)
    ap.add_argument("--restart", action="store_true")
    ap.add_argument("--restart-off", type=int, default=5)
    ap.add_argument("--rotate", action="store_true")
    ap.add_argument("--rotate-wait", type=int, default=600)
    ap.add_argument("--extra")
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--parent-wait", type=int, default=15)
    a = ap.parse_args()
    r = Run(a)
    try:
        r.main()
    finally:
        r.save()
        L.stop(r.d, "lfn")
        L.stop(r.d, "br")
        if a.mode == "p2w":
            W.stop_wsbrd(r.d)


if __name__ == "__main__":
    main()
