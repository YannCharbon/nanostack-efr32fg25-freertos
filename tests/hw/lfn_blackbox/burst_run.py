#!/usr/bin/env python3
"""Burst learning (stack 8138435, docs/LFN_MAC.md section 4) against Silicon Labs' LFN on the kits.

Silicon Labs' LFN takes one frame per listen window: on 2026-11-06 it acknowledged none of the
frames our parent sent right after a first one in the same window (frame-pending burst). Since
stack 8138435 the parent learns this per LFN and stops the bursts after 2 such frames are lost
with none acknowledged. This run checks that on hardware.

Set-up as s8_run.py --parent ours --profile balanced (listen 60 s): kit B runs our border router
(interop PHY, FW_LFN_PROFILE=balanced, a stack with 8138435), kit A Silicon Labs' wisun_soc_lfn_cli.
After the join, --rounds rounds, --gap s apart: the border router queues --count echo requests to
the LFN at once (10 ms apart), so the MAC has a second frame for the LFN when the first goes out.
The border router's FHSS traces (level debug, group "fhss") show each burst grant ("LFN burst to")
and the switch-off ("LFN ...: no bursts (2 burst frames lost, none acknowledged)").

Expected: burst grants in the first round only, the switch-off line once, then none; every echo
request answered, one per window (round trips about one listen interval apart).

usage: burst_run.py DIR [--rounds 3] [--count 3] [--gap 240] [--join-tmo 2700] [--settle 300]
Results: DIR/summary.txt, DIR/burst.json, the consoles' logs.
"""
import argparse
import json
import os
import re
import sys

T = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, T)
import p23lib as L  # noqa: E402
import s8_run as S  # noqa: E402

RX_GRANT = r"LFN burst to"
RX_OFF = r"LFN [0-9a-f:]+: no bursts \((\d+) burst frames lost, none acknowledged\)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--gap", type=int, default=240)
    ap.add_argument("--join-tmo", type=int, default=2700)
    ap.add_argument("--settle", type=int, default=300)
    a = ap.parse_args()
    s8a = argparse.Namespace(dir=a.dir, parent="ours", profile="balanced", join_tmo=a.join_tmo,
                             settle=0, duration=0, gap=a.gap, aem_every=0, aem_win=60, dump_at="",
                             dump_s=0, dry_run=False)
    r = S.S8(s8a)
    k, d = r.k, r.d
    out = {"rounds": [], "lines": []}

    def line(t):
        print(t, flush=True)
        out["lines"].append(t)

    try:
        if not r.parent_start():
            line("FAIL: border router did not start")
            return 1
        k.send("br", "trace 5 fhss")
        k.sleep(15)
        lfn = r.lfn_join()
        if not lfn:
            line("FAIL: LFN did not join")
            return 1
        line(f"FIGURE: LFN {lfn} connected after {r.res['join']['connected_s']} s")
        k.sleep(a.settle)   # the first exchanges after a join miss a window (2026-11-06 trial)
        wait = a.count * r.listen + 40
        for i in range(1, a.rounds + 1):
            k.check_consoles()
            p = k.size("br")
            k.send("br", f"ping {lfn} {a.count} 32 10 {wait}")
            k.sleep(wait + 15)
            txt = L.text(d, "br", p)
            rtts = [int(m.group(1)) / 1000 for m in re.finditer(S.rx_reply(lfn), txt)]
            grants = len(re.findall(RX_GRANT, txt))
            offs = [int(m.group(1)) for m in re.finditer(RX_OFF, txt)]
            rec = {"round": i, "replies": len(rtts), "rtt_s": rtts, "grants": grants, "off": offs}
            out["rounds"].append(rec)
            line(f"FIGURE: round {i}: {len(rtts)} of {a.count} answered, round trips {rtts} s; "
                 f"burst grants {grants}; switch-off lines {len(offs)}")
            if i < a.rounds:
                k.sleep(max(0, a.gap - wait - 15))
        rs = out["rounds"]
        later_grants = sum(x["grants"] for x in rs[1:])
        offs = sum(len(x["off"]) for x in rs)
        answered = sum(x["replies"] for x in rs)
        line(f"{'PASS' if rs[0]['grants'] >= 1 and offs == 1 else 'FAIL'}: bursts tried in round 1 "
             f"({rs[0]['grants']} grants) and switched off once ({offs} switch-off lines)")
        line(f"{'PASS' if later_grants == 0 else 'FAIL'}: no burst grant after the switch-off "
             f"({later_grants} in rounds 2-{a.rounds})")
        line(f"{'PASS' if answered == a.count * a.rounds else 'FAIL'}: echo requests answered "
             f"{answered} of {a.count * a.rounds}")
        lost = re.findall(S.RX_LOST, L.text(d, "lfn", getattr(r, "join_off", 0)))
        line(f"{'PASS' if not lost else 'FAIL'}: parent kept ({len(lost)} losses)")
        return 0 if all(x.startswith(("PASS", "FIGURE")) for x in out["lines"]) else 1
    finally:
        k.stop_consoles()
        json.dump(out, open(os.path.join(d, "burst.json"), "w"), indent=1)
        open(os.path.join(d, "summary.txt"), "w").write("\n".join(out["lines"]) + "\n")


if __name__ == "__main__":
    sys.exit(main())
