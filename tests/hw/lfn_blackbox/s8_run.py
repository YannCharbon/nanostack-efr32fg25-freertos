#!/usr/bin/env python3
"""S8 with Silicon Labs' real LFN profiles: their LFN (wisun_soc_lfn_cli, kit A,
440235509, the kit with a working AEM) with the Balanced or Eco profile, under
our border router (phase 3: wisun_br built with FW_LFN_PROFILE and the interop
PHY, kit B, 440240046) or under wsbrd + wisun_rcp (phase 1: kit B as RCP,
s8_<profile>.conf for the parent's LFN broadcast schedule). Root repo:
LFN_FAN11_Study.md §8.4 (S8) and §10.3; HW5_BlackBox_Phase1_Results.md §3.9, §5.6.

usage: s8_run.py DIR --parent {ours,wsbrd} --profile {balanced,eco}
                 [--join-tmo S] [--settle S] [--duration S] [--gap S]
                 [--aem-every S] [--aem-win S] [--dump-at S,S,...] [--dump-s S] [--dry-run]

The images are flashed beforehand (kit A: Silicon Labs' LFN; kit B: our
border router, or wisun_rcp_uart for wsbrd). Steps:
 1. consoles (kit A 9600 baud RTS/CTS, kit B 115200), the parent started
    (our border router reset, or wsbrd with the profile's LFN broadcast schedule);
 2. the LFN set up: network "hw5lfn", +10 dBm, the profile, saved; power cycle
    (a profile change needs a reboot, phase 1 item 39); credentials cleared;
    join_fan11; join time and states from its console;
 3. --settle s without traffic (the LFN's rate of the parent's clock);
 4. for --duration s: every --gap s one downlink (parent -> LFN echo request)
    and one uplink (LFN -> parent, its CLI 'wisun ping'), each awaited one listen
    interval + 20 s; an AEM window of --aem-win s on kit A every --aem-every s;
    AEM dumps (10 kHz) of --dump-s s at the offsets of --dump-at, analysed
    (aem_floor.py, aem_wakes.py: window periods);
 5. checks: joined; every downlink and uplink answered within one listen
    interval + 20 s (both directions are bounded by one unicast interval,
    phase 1 item 27); the parent kept (no new join state 1 after Connected);
    window periods in the dumps; AEM mean.
Results: DIR/results.json, DIR/summary.txt (PASS/FAIL and figure lines).
--dry-run prints every action without touching the kits.
Exit status 0 if every check passed.
"""
import argparse
import glob
import json
import os
import re
import signal
import subprocess
import sys
import time

T = os.path.dirname(os.path.abspath(__file__))
HW = os.path.abspath(os.path.join(T, ".."))
sys.path.insert(0, T)
import p23lib as L  # noqa: E402

LISTEN = {"balanced": 60, "eco": 300}        # the profiles' unicast listen interval (s)
BCAST = {"balanced": (60, 15), "eco": (600, 60)}   # parent: LFN broadcast interval (s), sync period
EUI_BR = "fd12:3456::b635:22ff:fe98:2391"    # our border router on kit B (prefix + EUI-64 IID)
# Console patterns (checked against the phase 3 logs of 2026-10-30/11-02, s8_parse_test)
RX_CONNECTED = r"\[Connected: (\d+) s\]|\[Failed[^\]]*\]"
RX_STATE = r"^(\d+\.\d+) .*\[Join state (\d)"
RX_ADDR = r"\[IPv6 address: ([0-9a-f:]+)\]"
RX_LOST = r"\[Join state 1|Connection lost|[Dd]isconnected"


def rx_reply(addr):
    """An echo reply line from addr, ours ('32 bytes from A: icmp_seq=1 time=N ms') or
    Silicon Labs' CLI ('40 bytes from A: icmp_seq=1 time=N ms')"""
    return re.escape(addr) + r": icmp_seq=\d+ time=(\d+) ms"


class Kits:
    """The kits' actions, or their printout in a dry run."""

    def __init__(self, d, dry):
        self.d, self.dry, self.vt, self.procs = d, dry, time.time(), {}

    def now(self):
        return self.vt if self.dry else time.time()

    def sleep(self, s):
        if self.dry:
            print(f"DRY: sleep {s:.0f}")
            self.vt += s
        else:
            time.sleep(s)

    def commander(self, *args):
        if self.dry:
            print("DRY: commander " + " ".join(args))
            return ""
        return L.commander(*args)

    def console(self, who, sn, baud, rtscts):
        """console.py on kit sn; restarted (appending) by check_consoles() if it exits."""
        if self.dry:
            print(f"DRY: console {who} on kit {sn} at {baud} baud{' RTS/CTS' if rtscts else ''} -> {who}.log")
            return
        self.procs[who] = (sn, baud, rtscts)
        L.console(self.d, who, sn, baud, rtscts)

    def check_consoles(self):
        if self.dry:
            return
        for who, (sn, baud, rtscts) in self.procs.items():
            p = L.pidfile(self.d, who)
            alive = False
            if os.path.exists(p):
                try:
                    os.kill(int(open(p).read()), 0)
                    alive = True
                except (ProcessLookupError, ValueError):
                    pass
            if not alive:
                with open(os.path.join(self.d, f"{who}.log"), "a") as f:
                    f.write(f"{time.time():.3f} === console restarted\n")
                try:
                    L.console(self.d, who, sn, baud, rtscts)
                except (IndexError, OSError):
                    pass   # tty not back yet: next time

    def stop_consoles(self):
        if not self.dry:
            for who in self.procs:
                L.stop(self.d, who)

    def send(self, who, cmd):
        if self.dry:
            print(f"DRY: {who} <- {cmd}")
            return
        try:
            fd = os.open(os.path.join(self.d, f"{who}.fifo"), os.O_WRONLY | os.O_NONBLOCK)
            os.write(fd, (cmd + "\n").encode())
            os.close(fd)
        except OSError as e:
            print(f"note: command '{cmd}' to {who} not delivered ({e})", flush=True)

    def size(self, who):
        return 0 if self.dry else L.size(self.d, who)

    def wait(self, who, pat, start, timeout):
        if self.dry:
            print(f"DRY: wait for /{pat}/ on {who}, up to {timeout:.0f} s")
            return None
        return L.wait(self.d, who, pat, start, timeout)

    def power_cycle(self, sn, off_s=6.0):
        if self.dry:
            print(f"DRY: commander adapter power -s {sn} off; sleep {off_s:.0f}; on")
            return
        L.power_cycle(sn, off_s)

    def aem(self, sn, s):
        if self.dry:
            print(f"DRY: commander aem measure -s {sn} --windowlength {int(s * 1000)}")
            self.vt += s
            return None
        ua, _ = L.aem_measure(sn, s)
        return ua

    def dump(self, sn, s, base):
        cmds = [["commander", "aem", "dump", "-s", sn, "--duration", str(int(s)), "-o", base + ".csv"],
                [sys.executable, os.path.join(HW, "aem_floor.py"), base + ".csv", "--json", base + ".json"],
                [sys.executable, os.path.join(HW, "aem_wakes.py"), "--line", base + ".json"]]
        if self.dry:
            for c in cmds:
                print("DRY: " + " ".join(os.path.basename(x) if x.endswith(".py") else x for x in c))
            self.vt += s
            return None
        out = ""
        for c in cmds:
            r = subprocess.run(c, capture_output=True, text=True)
            out = r.stdout
            if r.returncode and c[0] == "commander":
                return None
        subprocess.run(["gzip", "-f", base + ".csv"])
        return out.strip()


class S8:
    def __init__(self, a):
        self.a = a
        self.d = os.path.abspath(a.dir)
        os.makedirs(self.d, exist_ok=True)
        self.k = Kits(self.d, a.dry_run)
        self.res = {"parent": a.parent, "profile": a.profile, "join": None, "down": [], "up": [],
                    "aem_ua": [], "dumps": [], "lost_parent": [], "failures": [], "lines": []}
        self.listen = LISTEN[a.profile]

    def save(self):
        if not self.a.dry_run:
            json.dump(self.res, open(os.path.join(self.d, "results.json"), "w"), indent=1)

    def line(self, txt):
        print(txt, flush=True)
        self.res["lines"].append(txt)
        if txt.startswith("FAIL"):
            self.res["failures"].append(txt)
        self.save()

    # ------------------------------------------------------------- parent
    def parent_start(self):
        k = self.k
        if self.a.parent == "ours":
            k.commander("vcom", "config", "-s", L.B, "--baudrate", "115200", "--handshake", "disable")
            k.console("br", L.B, 115200, False)
            p = k.size("br")
            k.commander("device", "reset", "-s", L.B, "-d", L.DEV)
            r = k.wait("br", r"BR: interfaces up", p, 40)
            if not r and not k.dry:
                self.line("FAIL: S8 parent: our border router did not start")
                return False
            k.wait("br", r"LFN profile " + self.a.profile, p, 5)
            return True
        extra = os.path.join(T, f"s8_{self.a.profile}.conf")
        if k.dry:
            print(f"DRY: wsbrd.py start {self.d} --extra {extra} (kit B as RCP, fresh storage)")
            return True
        import wsbrd as W
        k.commander("vcom", "config", "-s", L.B, "--baudrate", "115200", "--handshake", "disable")
        _, t = W.start_wsbrd(self.d, extra, fresh_storage=True)
        if t is None:
            self.line("FAIL: S8 parent: wsbrd did not start (see wsbrd.log)")
            return False
        return True

    def parent_stop(self):
        if self.a.parent == "wsbrd":
            if self.k.dry:
                print("DRY: wsbrd.py stop")
            else:
                import wsbrd as W
                W.stop_wsbrd(self.d)

    def parent_addr(self):
        if self.a.parent == "ours":
            return EUI_BR
        if self.k.dry:
            return "fd00:5eed::<tunlfn>"
        out = subprocess.run(["ip", "-6", "addr", "show", "tunlfn"], capture_output=True, text=True).stdout
        m = re.search(r"inet6 (fd00:5eed:[0-9a-f:]+)/", out)
        return m.group(1) if m else None

    # ---------------------------------------------------------------- LFN
    def lfn_boot(self):
        k = self.k
        for attempt in range(3):
            p = k.size("lfn")
            k.power_cycle(L.A, 6.0 + 4 * attempt)
            if k.dry or k.wait("lfn", r"Wi-SUN LFN CLI Application", p, 15):
                return True
        return False

    def lfn_join(self):
        k, a = self.k, self.a
        k.commander("vcom", "config", "-s", L.A, "--baudrate", "9600", "--handshake", "rtscts")
        k.console("lfn", L.A, 9600, True)
        if not self.lfn_boot():
            self.line("FAIL: S8 LFN: no boot banner after 3 power cycles")
            return None
        k.sleep(2)
        for cmd in ('wisun set wisun.network_name "hw5lfn"', "wisun set wisun.tx_power_ddbm 100",
                    f"wisun set wisun.lfn_profile {a.profile}", "wisun save"):
            k.send("lfn", cmd)
            k.sleep(1.5)
        self.lfn_boot()               # the profile is applied at boot (phase 1 item 39)
        k.sleep(2)
        p = k.size("lfn")
        k.send("lfn", "wisun get wisun.lfn_profile")
        r = k.wait("lfn", r"wisun\.lfn_profile = (\w+)", p, 10)
        if r and r[0].group(1) != a.profile:
            self.line(f"FAIL: S8 LFN profile is {r[0].group(1)}, not {a.profile}")
            return None
        k.send("lfn", "wisun clear_credential_cache")
        k.sleep(1.5)
        p = k.size("lfn")
        t0 = k.now()
        k.send("lfn", "wisun join_fan11")
        r = k.wait("lfn", RX_CONNECTED, p, a.join_tmo)
        if k.dry:
            self.res["join"] = {"connected_s": 0}
            return "fd12:3456::<lfn>" if a.parent == "ours" else "fd00:5eed::<lfn>"
        if not r or not r[0].group(0).startswith("[Connected"):
            self.line(f"FAIL: S8 {a.profile} under {a.parent}: LFN not connected within {a.join_tmo} s")
            return None
        txt = L.text(self.d, "lfn", p)
        states = [(float(m.group(1)) - t0, m.group(2)) for m in re.finditer(RX_STATE, txt, re.M)]
        addr = re.search(RX_ADDR, txt)
        self.res["join"] = {"connected_s": int(r[0].group(1)), "host_s": round(r[1] - t0, 1),
                            "states": [(round(t, 1), s) for t, s in states]}
        self.join_off = L.size(self.d, "lfn")
        return addr.group(1) if addr else None

    # ------------------------------------------------------------ traffic
    def downlink(self, lfn, i):
        k, wait = self.k, self.listen + 20
        t0 = k.now()
        if self.a.parent == "ours":
            p = k.size("br")
            k.send("br", f"ping {lfn} 1 32 1000 {wait}")
            r = k.wait("br", rx_reply(lfn), p, wait + 5)
            rtt = int(r[0].group(1)) / 1000 if r else None
        elif k.dry:
            print(f"DRY: ping -6 -c 1 -W {wait} -I tunlfn {lfn}")
            rtt = None
        else:
            pr = subprocess.run(["ping", "-6", "-c", "1", "-W", str(wait), "-I", "tunlfn", lfn],
                                capture_output=True, text=True)
            m = re.search(r"time=([0-9.]+) ms", pr.stdout)
            rtt = float(m.group(1)) / 1000 if m else None
        self.res["down"].append({"i": i, "t": t0, "rtt_s": rtt})
        print(f"  downlink {i}: {rtt}", flush=True)

    def uplink(self, parent, i):
        k, wait = self.k, self.listen + 20
        p = k.size("lfn")
        t0 = k.now()
        k.send("lfn", f"wisun ping {parent}")
        r = k.wait("lfn", rx_reply(parent), p, wait)
        rtt = int(r[0].group(1)) / 1000 if r else None
        self.res["up"].append({"i": i, "t": t0, "rtt_s": rtt})
        print(f"  uplink {i}: {rtt}", flush=True)

    # --------------------------------------------------------------- main
    def run(self):
        a, k = self.a, self.k
        if not self.parent_start():
            return
        k.sleep(15)
        lfn = self.lfn_join()
        if not lfn:
            return
        j = self.res["join"]
        self.line(f"FIGURE: S8 {a.profile} under {a.parent}: LFN connected after {j['connected_s']} s "
                  f"(join states at {j.get('states', [])})")
        parent = self.parent_addr()
        k.sleep(a.settle)
        t_end = k.now() + a.duration
        dumps = sorted(int(x) for x in a.dump_at.split(",") if x) if a.dump_at else []
        t_start = k.now()
        next_tr, next_aem, i = k.now(), k.now() + 60, 0
        while k.now() < t_end:
            k.check_consoles()
            el = k.now() - t_start
            if dumps and el >= dumps[0]:
                dumps.pop(0)
                base = os.path.join(self.d, f"aem_{L.A}_{len(self.res['dumps']) + 1}")
                out = k.dump(L.A, a.dump_s, base)
                self.res["dumps"].append(out)
                print(f"  dump: {out}", flush=True)
            elif k.now() >= next_tr:
                i += 1
                self.downlink(lfn, i)
                k.sleep(5)
                self.uplink(parent, i)
                next_tr = k.now() + a.gap
            elif a.aem_every and k.now() >= next_aem:
                ua = k.aem(L.A, a.aem_win)
                self.res["aem_ua"].append((k.now(), ua))
                next_aem = k.now() + a.aem_every
            else:
                k.sleep(min(5, max(1, t_end - k.now())) if not k.dry else
                        max(1, min(next_tr, next_aem, t_end) - k.now()))
            self.save()
        self.verdict(lfn)

    def verdict(self, lfn):
        a, r = self.a, self.res
        if self.k.dry:
            print(f"DRY: checks: downlinks and uplinks answered within {self.listen + 20} s, parent kept, "
                  "AEM windows, dump window periods")
            return
        tag = f"S8 {a.profile} under {a.parent}"
        lim = self.listen + 20
        for way in ("down", "up"):
            xs = r[way]
            ok = [x for x in xs if x["rtt_s"] is not None and x["rtt_s"] <= lim]
            rtts = sorted(x["rtt_s"] for x in xs if x["rtt_s"] is not None)
            rng = f"{rtts[0]:.1f}-{rtts[-1]:.1f} s" if rtts else "none"
            self.line(f"{'PASS' if xs and len(ok) == len(xs) else 'FAIL'}: {tag}: "
                      f"{'downlinks' if way == 'down' else 'uplinks'} {len(ok)} of {len(xs)} answered within "
                      f"one listen interval + 20 s ({lim} s); round trips {rng}")
        if not self.k.dry:
            txt = L.text(self.d, "lfn", getattr(self, "join_off", 0))
            lost = re.findall(RX_LOST, txt)
            self.line(f"{'PASS' if not lost else 'FAIL'}: {tag}: parent kept for the run ({len(lost)} losses)")
        ua = [u for _, u in r["aem_ua"] if u is not None]
        if ua:
            self.line(f"FIGURE: {tag}: AEM {len(ua)} windows of {a.aem_win} s: {min(ua):.1f}-{max(ua):.1f} uA, "
                      f"mean {sum(ua) / len(ua):.1f} uA")
        elif a.aem_every:
            self.line(f"FAIL: {tag}: no AEM reading")
        for out in r["dumps"]:
            if out:
                self.line(f"FIGURE: {tag}: dump {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--parent", choices=["ours", "wsbrd"], required=True)
    ap.add_argument("--profile", choices=["balanced", "eco"], required=True)
    ap.add_argument("--join-tmo", type=int, default=3600)
    ap.add_argument("--settle", type=int, default=1800)
    ap.add_argument("--duration", type=int, default=5400)
    ap.add_argument("--gap", type=int, default=900)
    ap.add_argument("--aem-every", type=int, default=600)
    ap.add_argument("--aem-win", type=int, default=60)
    ap.add_argument("--dump-at", default="")
    ap.add_argument("--dump-s", type=int, default=300)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    s = S8(a)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    try:
        s.run()
    finally:
        s.parent_stop()
        s.k.stop_consoles()
        s.save()
        if not a.dry_run:
            open(os.path.join(s.d, "summary.txt"), "w").write("\n".join(s.res["lines"]) + "\n")
    rc = 1 if s.res["failures"] else 0
    print("RESULT:", "PASS" if rc == 0 else f"FAIL ({len(s.res['failures'])})", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
