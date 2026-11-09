#!/usr/bin/env python3
"""Two-frame collision measurements on three FG25 kits running RAILtest.

Roles (kit serials, BRD4271A 868 MHz boards):
  R  receiver; also sends the short trigger frame that starts each cycle
  W  "wanted" transmitter
  I  interferer
W and I wait in RX with RAILtest's `txAfterRx <d>`: each sends one frame
d microseconds after the sync-word end of the trigger it receives, so the
relative start time of the two frames is set to the microsecond by dW and dI
(calibrated with `calib`). Both radios go idle after the trigger (no
receiving in between, so a frame on air cannot postpone their TX) and after
their TX; the host re-arms them every cycle. R returns to RX right after
sending the trigger and reports every frame it syncs on, CRC failures
included (RX options IGNORE_CRC_ERRORS | TRACK_ABORTED).

Each cycle is checked on W's and I's consoles: the frame each one received
before transmitting must be R's trigger (payload marker 0xCC) and the TX must
have completed; cycles that fail this check (e.g. a Wi-SUN frame from the lab
caught instead of the trigger) are recorded as invalid and not counted.

Frames are told apart at R by payload marker (W 0xAA, I 0xBB, bytes 2-3 after
the PHR) and by their sync-end time relative to the trigger. Output: one
JSON line per cycle (raw), and every console line in <out>/console.log.

usage: cochannel.py <experiment> <out dir> [options]   (see main())
"""
import argparse
import json
import os
import random
import sys
import time

from rt import RT, parse

SN = {"R": 440240046, "W": 440235509, "I": 440290182}
MARK = {"W": 0xAA, "I": 0xBB, "T": 0xCC}
# Wi-SUN EU mode 3 (150 kbit/s): 12-byte preamble + 2-byte SFD before the
# sync end; RAILtest's "len" counts PHR + payload, the 4-byte FCS follows.
BITRATE = 150000


def us_bytes(n, bitrate=None):
    return n * 8e6 / (bitrate or BITRATE)


class Bench:
    def __init__(self, out, bitrate=BITRATE, shr_bytes=14, channel=0):
        os.makedirs(out, exist_ok=True)
        self.out = out
        self.bitrate = bitrate
        self.shr_bytes = shr_bytes
        self.channel = channel
        log = os.path.join(out, "console.log")
        self.k = {n: RT(SN[n], log=log, name=n) for n in SN}
        self.cfg = {}
        self.rstream = []

    # ---- setup -------------------------------------------------------
    def setup(self, trig_len=10, trig_pwr=-150):
        for n, k in self.k.items():
            k.cmd("reset", 1.5)
        time.sleep(0.3)
        for n, k in self.k.items():
            k.lines(0.2)
            k.cmd("rx 0")
            k.cmd("setNotifications 1")
            k.cmd(f"setChannel {self.channel}")
        R = self.k["R"]
        R.cmd("setRxOptions 10")            # ignore CRC errors, track aborted
        R.cmd("setTxTransitions rx rx")
        R.cmd("setRxTransitions rx rx")
        R.cmd(f"setPower {trig_pwr}")
        self.set_frame("R", trig_len, MARK["T"])
        for n in ("W", "I"):
            k = self.k[n]
            k.cmd("setTxTransitions idle idle")
            k.cmd("setRxTransitions idle idle")
        R.cmd("rx 1")

    def set_frame(self, n, length, mark):
        k = self.k[n]
        k.cmd(f"setTxLength {length}")
        k.cmd("set802154PHR 1 0 1")          # SUN FSK PHR, 4-byte FCS, whitening
        k.cmd(f"setTxPayloadQuiet 2 {mark} {mark}")
        self.cfg[n + "_len"] = length

    def set_power(self, n, ddbm):
        k = self.k[n]
        k.cmd("txAfterRx 0")
        k.cmd("rx 0")
        r = k.cmd(f"setPower {ddbm}")
        got = [d.get("power") for m, d in r if m == "setPower"]
        if not got or got[0] is None:
            raise RuntimeError(f"setPower {ddbm} on {n}: {r}")
        self.cfg[n + "_pwr"] = int(got[0])
        return int(got[0])

    # ---- one cycle -----------------------------------------------------
    def _wait_for(self, n, pred, timeout, acc):
        t_end = time.time() + timeout
        while time.time() < t_end:
            for line in self.k[n].lines(0.0):
                acc.append(line)
            if any(pred(parse(x)) for x in acc):
                return True
            time.sleep(0.001)
        return False

    def cycle(self, dW, dI):
        """dW/dI: delay after the trigger's sync end, None = that kit silent."""
        k = self.k
        armed = []
        for n, d in (("W", dW), ("I", dI)):
            k[n].lines(0.0)                  # flush
            k[n].send("txAfterRx 0")
            if d is not None:
                k[n].send(f"txAfterRx {int(d)}")
                armed.append(n)
        rec = {"t": round(time.time(), 3), "dW": dW, "dI": dI, "rx": [], "valid": True}
        for n in armed:
            acc = []
            if not self._wait_for(n, lambda pd: pd[0] == "txAfterRx" and pd[1].get("SchTxAfterRx") == "Enabled",
                                  0.3, acc):
                rec["valid"] = False
                rec[n + "_arm"] = False
        self.rstream += k["R"].lines(0.0)
        k["R"].send("tx 1")
        span = max([d for d in (dW, dI) if d is not None] + [0]) * 1e-6
        lw = max(self.cfg.get("W_len", 0), self.cfg.get("I_len", 0))
        span += us_bytes(lw + 4 + self.shr_bytes, self.bitrate) * 1e-6
        time.sleep(span + 0.004)
        acc = {n: [] for n in ("R", "W", "I")}
        for n in armed:
            self._wait_for(n, lambda pd: pd[0] == "txEnd", 0.15, acc[n])
        self._wait_for("R", lambda pd: pd[0] == "txEnd", 0.1, acc["R"])
        self.rstream += acc["R"]
        for line in acc["R"]:
            m, d = parse(line)
            if m == "txEnd" and d.get("txStatus") == "Complete":
                rec["trig_start"] = int(d.get("lastTxStart", 0))
                rec["trig_end"] = int(d.get("lastTxTime", 0))
        for n in armed:
            ok_trig = ok_tx = False
            for line in acc[n]:
                m, d = parse(line)
                if m == "rxPacket":
                    p = d.get("payload", "").split()
                    ok_trig = len(p) >= 3 and int(p[2], 16) == MARK["T"]
                elif m == "txEnd":
                    ok_tx = d.get("txStatus") == "Complete"
                    rec[n + "_txdur"] = int(d.get("durationUs", 0))
            rec[n + "_ok"] = ok_tx and ok_trig
            if not rec[n + "_ok"]:
                rec["valid"] = False
        if "trig_end" not in rec:
            rec["valid"] = False
        return rec


    def assign(self, recs, window_us=40000):
        """Attach R's frame reports to the cycles by R's own timestamps: a
        frame belongs to the cycle whose trigger ended last before it."""
        self.rstream += self.k["R"].lines(0.15)
        frames = []
        for line in self.rstream:
            m, d = parse(line)
            if m and m.startswith("rx") and m != "rx" and "timeUs" in d:
                p = d.get("payload", "").split()
                frames.append({"st": m, "len": int(d.get("len", 0)), "t": int(d.get("timeUs", 0)),
                               "crc": d.get("crc"), "rssi": int(d["rssi"]) if d.get("rssi") else None,
                               "p": [int(x, 16) for x in p[:4]]})
        self.rstream = []
        for r in recs:
            r["rx"] = []
            te = r.get("trig_end")
            if te is None:
                continue
            for f in frames:
                # RAIL time is 32-bit microseconds; wraps after 71.6 min
                rel = (f["t"] - te) % (1 << 32)
                if rel < window_us:
                    r["rx"].append(f)
        return recs


def classify(rec, offset_us=443, tol_us=100):
    """Outcome at R of each frame of a cycle: 'ok' (received, CRC good,
    right marker), 'bad' (R synced at that frame's time but the frame failed:
    CRC error, wrong length or aborted), 'miss' (no sync at that time), None
    (kit not armed). A frame of kit n is expected at R with its sync end
    d_n + offset_us after the trigger's end (calibrated: 441-447 us)."""
    res = {}
    for n in ("W", "I"):
        d = rec.get("d" + n)
        res[n] = None if d is None else "miss"
    te = rec.get("trig_end")
    if te is None:
        return res
    for f in rec["rx"]:
        rel = (f["t"] - te) % (1 << 32)
        mark = f["p"][2] if len(f["p"]) > 2 else None
        for n in ("W", "I"):
            if res[n] is None:
                continue
            if abs(rel - (rec["d" + n] + offset_us)) <= tol_us:
                if f["st"] == "rxPacket" and f["crc"] == "Pass" and mark == MARK[n]:
                    res[n] = "ok"
                elif res[n] != "ok":
                    res[n] = "bad"
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp")
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--wlen", type=int, default=50)
    ap.add_argument("--ilen", type=int, default=50)
    ap.add_argument("--dW", type=int, default=3000)
    ap.add_argument("--dI", type=int, default=3000)
    ap.add_argument("--pW", type=int, default=-150)
    ap.add_argument("--pI", type=int, default=-150)
    a = ap.parse_args()
    b = Bench(a.out)
    b.setup()
    b.set_frame("W", a.wlen, MARK["W"])
    b.set_frame("I", a.ilen, MARK["I"])
    b.set_power("W", a.pW)
    b.set_power("I", a.pI)
    with open(os.path.join(a.out, f"{a.exp}.jsonl"), "a") as f:
        for i in range(a.n):
            for dW, dI in ((a.dW, None), (None, a.dI), (a.dW, a.dI)):
                r = b.cycle(dW, dI)
                r.update(exp=a.exp, wlen=a.wlen, ilen=a.ilen, pW=a.pW, pI=a.pI)
                f.write(json.dumps(r) + "\n")
                print(json.dumps(r))


if __name__ == "__main__":
    main()
