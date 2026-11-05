#!/usr/bin/env python3
"""Summary of the weekend LFN campaign (run_weekend.sh): one line per check.

usage: tests/hw/weekend_summary.py OUT_DIR   (writes OUT_DIR/summary.txt and prints it)

Reads each step directory of OUT_DIR as its script wrote it:
- run_hw_gs11.sh (A1_*): pairs.txt and its PASS/FAIL lines;
- hwlib aem_dump (A2_*, and the dumps inside other steps): *.wakes.txt;
- run_hw_lfn_dl.sh (A3_*, BX*, C*): out.txt, latencies.txt, aem.txt;
- lfn_blackbox/s8_run.py (BY*): summary.txt or results.json.
Every PASS/FAIL line of a step is kept, prefixed with the step; FIGURE lines
add the measured values, with the reference figures of the documents where
they exist. A step's cmd.txt (its command line) gives LISTEN_S.
Lines: "PASS: ...", "FAIL: ...", "FIGURE: ...", "NOTE: ...".
"""
import glob
import os
import re
import statistics as st
import sys


def rd(p):
    try:
        return open(p, errors="replace").read()
    except OSError:
        return ""


def rng(xs, fmt="{:.1f}"):
    xs = sorted(xs)
    return (fmt.format(xs[0]) + ("" if len(xs) == 1 else "-" + fmt.format(xs[-1]))) if xs else "none"


def env_of(d, key, default):
    m = re.search(r"\b" + key + r"=(\S+)", rd(os.path.join(d, "cmd.txt")))
    return m.group(1) if m else default


def own_checks(tag, out):
    return [f"{l.split(':', 1)[0]}: {tag}: {l.split(':', 1)[1].strip()}" for l in out.splitlines()
            if re.match(r"(PASS|FAIL): ", l)]


def step_exit(tag, out):
    m = re.search(r"^exit (\d+)", out, re.M)
    if not out:
        return [f"NOTE: {tag}: no output (not run)"]
    if not m:
        return [f"NOTE: {tag}: still running or stopped before its end"]
    return []


def consoles(tag, d):
    n = sum(rd(p).count("=== console restarted") for p in glob.glob(os.path.join(d, "*.log")))
    lost = sum(rd(p).count("=== console lost") for p in glob.glob(os.path.join(d, "*.log")))
    return [f"NOTE: {tag}: consoles lost {lost} time(s), restarted {n} time(s)"] if (n or lost) else []


def wakes(tag, d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "*.wakes.txt"))):
        txt = rd(p).strip()
        if not txt:
            continue
        m = re.search(r"aem_(\d+)_\d+", os.path.basename(p))
        kit = m.group(1) if m else "?"
        for line in txt.splitlines():
            out.append(f"FIGURE: {tag}: AEM dump kit {kit}: {line.split(': ', 1)[-1]}")
            t = re.search(r"timer wakes ([0-9.]+)/min", line)
            if t and kit == "440290182":
                v = float(t.group(1))
                # README "Housekeeping wakes removed": 4.0 a minute before stack aa2a828, 1.0 after
                out.append(f"{'PASS' if v < 2.0 else 'FAIL'}: {tag}: housekeeping change on kit {kit}: "
                           f"{v} timer wakes a minute (1.0 expected since stack aa2a828, 4.0 before)")
    return out


def gs11(tag, d):
    out = own_checks(tag, rd(os.path.join(d, "out.txt")))
    rows = [l.split() for l in rd(os.path.join(d, "pairs.txt")).splitlines() if l.strip()]
    ja = [float(r[1]) for r in rows if r[1] != "none"]
    jb = [float(r[2]) for r in rows if r[2] != "none"]
    off = [float(r[3]) for r in rows if len(r) > 3 and re.match(r"^\d+$", r[3])]
    if rows:
        out.append(f"FIGURE: {tag}: {len(rows)} paired resets: join times {rng(ja)} s and {rng(jb)} s "
                   f"(kits so far, one LFN at a time: 29-74 s cold, 8-33 s with stored keys); "
                   f"boot offsets {rng(off, '{:.0f}')} ms (a collision needs about 5 ms)")
    return out


def dl(tag, d):
    """run_hw_lfn_dl.sh outputs"""
    o = rd(os.path.join(d, "out.txt"))
    out = own_checks(tag, o)
    listen = float(env_of(d, "LISTEN_S", "5"))
    for m in re.finditer(r"^(lfn\d+) joined, (\S+), parent (\S+)(?:, (\d+) s after the start of the wait)?", o, re.M):
        out.append(f"FIGURE: {tag}: {m.group(1)} joined" + (f" {m.group(4)} s after its reset" if m.group(4) else "")
                   + f", parent {m.group(3)}")
    m = re.search(r"router: operational at (\d+) ms, LGTK at (\d+) ms", o)
    if m:
        out.append(f"FIGURE: {tag}: router operational at {int(m.group(1)) / 1000:.0f} s, LGTK at "
                   f"{int(m.group(2)) / 1000:.0f} s after its reset (2026-11-04: LGTK 36.6-125.9 s after a cold start)")
    lat = [l.split() for l in rd(os.path.join(d, "latencies.txt")).splitlines() if l.strip()]
    for lfn in sorted({r[0] for r in lat}):
        rows = [r for r in lat if r[0] == lfn]
        got = [float(r[4]) for r in rows if r[4] != "none"]
        late = [r for r in rows if r[4] != "none" and float(r[4]) > listen + 1.0]
        out.append(f"FIGURE: {tag}: {lfn}: {len(got)} of {len(rows)} downlinks answered, latency {rng(got)} s, "
                   f"{len(late)} later than one listen interval ({listen:.0f} s) + 1 s"
                   + (" (overnight run 2026-11-04, listen 5 s: 31 of 32, 0.3-6.0 s)" if listen == 5 else ""))
        for r in rows:
            if float(r[2]) >= 3600:
                ok = r[4] != "none"
                out.append(f"{'PASS' if ok else 'FAIL'}: {tag}: {lfn}: downlink {r[1]} after {float(r[2]) / 3600:.1f} h "
                           f"of silence {'answered in ' + format(float(r[4]), '.1f') + ' s' if ok else 'not answered'}")
    m = re.search(r"after the router reset: (.*)", o)
    if m:
        t = re.search(r"answered (\d+) s after it", m.group(1))
        n = re.search(r"(\d+) datagram\(s\) unanswered", m.group(1))
        out.append(f"FIGURE: {tag}: router restart: traffic back {t.group(1) + ' s' if t else 'never'} after the reset, "
                   f"{n.group(1) if n else '?'} datagram(s) lost (2026-11-04: 97-194 s, 1-3 lost per run, DI8)")
    aem = [l.split() for l in rd(os.path.join(d, "aem.txt")).splitlines() if l.strip()]
    kits = {}
    for r in aem:
        kit, v = (r[1], r[2]) if len(r) >= 3 else (env_of(d, "AEM_SN", "?"), r[1])
        if v != "none":
            kits.setdefault(kit, []).append(float(v))
    for kit, vs in sorted(kits.items()):
        out.append(f"FIGURE: {tag}: AEM kit {kit}: {len(vs)} windows {rng(vs, '{:.2f}')} uA, mean {st.mean(vs):.2f} uA")
    out += wakes(tag, d)
    return out


def s8(tag, d):
    txt = rd(os.path.join(d, "summary.txt"))
    if not txt:
        import json
        try:
            txt = "\n".join(json.load(open(os.path.join(d, "results.json")))["lines"])
        except (OSError, ValueError, KeyError):
            txt = ""
    return [f"{l.split(':', 1)[0]}: {tag}: {l.split(':', 1)[1].strip().removeprefix('S8 ')}"
            for l in txt.splitlines() if re.match(r"(PASS|FAIL|FIGURE): ", l)]


def main():
    od = sys.argv[1]
    lines = []
    for d in sorted(glob.glob(os.path.join(od, "[A-Z]*_*"))):
        if not os.path.isdir(d):
            continue
        tag = os.path.basename(d)
        o = rd(os.path.join(d, "out.txt"))
        if "gs11" in tag:
            lines += gs11(tag, d)
        elif tag.startswith("A2"):
            lines += wakes(tag, d)
        elif tag.startswith("BY"):
            lines += s8(tag, d)
        else:
            lines += dl(tag, d)
        if not tag.startswith("A2"):
            lines += step_exit(tag, o)
        lines += consoles(tag, d)
    np, nf = sum(l.startswith("PASS") for l in lines), sum(l.startswith("FAIL") for l in lines)
    lines.append(f"TOTAL: {np} PASS, {nf} FAIL")
    txt = "\n".join(lines) + "\n"
    try:
        open(os.path.join(od, "summary.txt"), "w").write(txt)
    except OSError:
        pass
    print(txt, end="")
    return 1 if nf else 0


if __name__ == "__main__":
    sys.exit(main())
