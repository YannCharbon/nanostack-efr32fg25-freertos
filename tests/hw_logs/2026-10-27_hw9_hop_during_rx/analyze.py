#!/usr/bin/env python3
"""Summarise HW9 campaign runs: analyze.py <runs dir>"""
import os, re, sys, statistics as st

R = sys.argv[1]
rows = []
for d in sorted(os.listdir(R)):
    p = os.path.join(R, d)
    try:
        rn = open(os.path.join(p, "rn.log"), errors="replace").read().splitlines()
        br = open(os.path.join(p, "br.log"), errors="replace").read().splitlines()
    except OSError:
        continue
    # keep the RN's last boot only (the RN may print a first boot before the reset)
    starts = [i for i, l in enumerate(rn) if "RN starting" in l]
    if starts:
        rn = rn[starts[-1]:]
    def ms(lines, pat):
        for l in lines:
            if re.search(pat, l):
                m = re.search(r"\[(\d+)\] \[", l)
                if m:
                    return int(m.group(1)) / 1000
        return None
    ready = ms(rn, r"network event on if 1: ARM_NWK_BOOTSTRAP_READY")
    parent = ms(rn, r"selected parent")
    auth = ms(rn, r"EAP-TLS finished")
    eaps = [l for l in rn if "[eaps]" in l]
    last = re.sub(r"^.*\[eaps\]: ", "", eaps[-1]) if eaps else "-"
    retries = sum("EAP-TLS: retry REQ" in l for l in br)
    big = [re.search(r"id (\d+) flags \w+ len (\d+)", l).groups() for l in br if "retry REQ" in l]
    hops_rn = [l for l in rn if "during RX" in l]
    hops_br = [l for l in br if "during RX" in l]
    def stat(lines, key):
        for l in reversed(lines):
            m = re.search(key + r"=(\d+)", l)
            if m:
                return int(m.group(1))
        return None
    rows.append(dict(run=d, v=d.split("-")[0], ok=ready is not None, ready=ready, parent=parent, auth=auth,
                     last=last[:60], retries=retries, big=sorted(set(big)),
                     hops_rn=len(hops_rn), hops_br=len(hops_br), hops_rn_l=hops_rn, hops_br_l=hops_br,
                     txlost=(stat(rn, "tx_lost"), stat(br, "tx_lost"))))
for r in rows:
    print(f"{r['run']:6} {'JOIN ' if r['ok'] else 'STALL'} ready={r['ready']} parent={r['parent']} auth={r['auth']} "
          f"br_retry={r['retries']} {r['big']} hopRX rn={r['hops_rn']} br={r['hops_br']} tx_lost={r['txlost']}"
          + ("" if r['ok'] else f" last_rn_eap='{r['last']}'"))
print()
for v in sorted(set(r["v"] for r in rows)):
    rs = [r for r in rows if r["v"] == v]
    j = [r["ready"] for r in rs if r["ok"]]
    s = f"{v}: boots={len(rs)} stalls={sum(not r['ok'] for r in rs)}"
    if j:
        s += f" join(RN uptime s) median={st.median(j):.0f} mean={st.mean(j):.0f} min={min(j):.0f} max={max(j):.0f}"
    s += f" boots-with-hop-during-RX rn={sum(r['hops_rn']>0 for r in rs)} br={sum(r['hops_br']>0 for r in rs)}"
    s += f" total hop events rn={sum(r['hops_rn'] for r in rs)} br={sum(r['hops_br'] for r in rs)}"
    print(s)
