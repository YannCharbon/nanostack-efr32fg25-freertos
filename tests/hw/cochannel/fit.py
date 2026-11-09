#!/usr/bin/env python3
"""Logistic fit of frame loss vs SIR, from sweep.py results.

loss(x) = 1 / (1 + exp((x - x50) / s)), x = SIR of the frame in dB
(RSSI of that frame alone minus RSSI of the other frame alone, both at R).
Maximum likelihood by grid search (no SciPy needed). Prints x50, s and the
SIR for 10 % and 1 % loss, with the pooled points.

usage: fit.py <jsonl>... [--frame first|second]
  first : the frame that started first (I); its SIR = -(SIR of W)
  second: the later frame (W)
"""
import math
import sys

from analyze import points


def fit(pts):
    best = None
    for i in range(0, 501):
        x50 = -10 + i * 0.05
        for j in range(1, 151):
            s = j * 0.02
            ll = 0.0
            for x, k, n in pts:
                z = (x - x50) / s
                p = 1 / (1 + math.exp(-z)) if z > -50 else 0.0   # success probability
                p = min(max(p, 1e-9), 1 - 1e-9)
                ll += k * math.log(p) + (n - k) * math.log(1 - p)
            if best is None or ll > best[0]:
                best = (ll, x50, s)
    return best


def main():
    files = [a for a in sys.argv[1:] if a.endswith(".jsonl")]
    frame = "second" if "--frame" in sys.argv and sys.argv[sys.argv.index("--frame") + 1] == "second" else "first"
    pts = []
    for fn in files:
        for p in points(fn):
            if p["sir"] is None or p["n"] == 0:
                continue
            if frame == "first":
                pts.append((-p["sir"], p["I_ok"], p["n"]))
            else:
                pts.append((p["sir"], p["W_ok"], p["n"]))
    pts.sort()
    ll, x50, s = fit(pts)
    print(f"frame={frame} points={len(pts)} cycles={sum(n for _, _, n in pts)}")
    print(f"x50={x50:.2f} dB  s={s:.2f} dB  loss10%={x50 + s * math.log(9):.2f} dB  loss1%={x50 + s * math.log(99):.2f} dB")
    for x, k, n in pts:
        z = (x - x50) / s
        print(f"  SIR {x:+6.1f}  received {k:4d}/{n:<4d} ({100 * k / n:5.1f} %)  fit {100 / (1 + math.exp(-z)):5.1f} %")


if __name__ == "__main__" and "--ber" not in __import__("sys").argv:
    main()


def ber_fit(groups):
    """Joint fit over frame lengths of P_ok = (1 - BER)^bits with
    BER = min(0.5, 0.5 * exp(-(x - x0) / b)): the emulator's form with the
    interference SIR x in dB, a shift x0 and a scale b (emulator: x0 = 0,
    b = 2). groups: list of (bits, [(x, k, n)])."""
    best = None
    for i in range(0, 401):
        x0 = -10 + i * 0.05
        for j in range(5, 301):
            b = j * 0.01
            ll = 0.0
            for bits, pts in groups:
                for x, k, n in pts:
                    ber = min(0.5, 0.5 * math.exp(-(x - x0) / b))
                    p = (1 - ber) ** bits
                    p = min(max(p, 1e-12), 1 - 1e-12)
                    ll += k * math.log(p) + (n - k) * math.log(1 - p)
            if best is None or ll > best[0]:
                best = (ll, x0, b)
    return best


def ber_main(argv):
    """fit.py --ber <bits>:<jsonl>[,<jsonl>] ... (first frame)"""
    groups = []
    for a in argv:
        if ":" not in a:
            continue
        bits, fns = a.split(":", 1)
        pts = []
        for fn in fns.split(","):
            for p in points(fn):
                if p["sir"] is not None and p["n"]:
                    pts.append((-p["sir"], p["I_ok"], p["n"]))
        groups.append((int(bits), pts))
    ll, x0, b = ber_fit(groups)
    print(f"form 1 (emulator's): BER = min(0.5, 0.5*exp(-(SIR_dB - {x0:.2f}) / {b:.2f}))  logL={ll:.1f}")
    for bits, pts in groups:
        for target in (0.5, 0.9, 0.99):
            ber = 1 - target ** (1 / bits)
            x = x0 - b * math.log(ber / 0.5)
            print(f"  {bits} bits: {100 * target:.0f} % received at SIR {x:.2f} dB")
    ll2, g = lin_fit(groups)
    print(f"form 2 (non-coherent FSK): BER = 0.5*exp(-{g:.3f} * SIR_linear)  logL={ll2:.1f}")
    for bits, pts in groups:
        for target in (0.5, 0.9, 0.99):
            ber = 1 - target ** (1 / bits)
            x = 10 * math.log10(-math.log(2 * ber) / g)
            print(f"  {bits} bits: {100 * target:.0f} % received at SIR {x:.2f} dB")


def lin_fit(groups):
    best = None
    for i in range(1, 4001):
        g = i * 0.001
        ll = 0.0
        for bits, pts in groups:
            for x, k, n in pts:
                ber = min(0.5, 0.5 * math.exp(-g * 10 ** (x / 10)))
                p = (1 - ber) ** bits
                p = min(max(p, 1e-12), 1 - 1e-12)
                ll += k * math.log(p) + (n - k) * math.log(1 - p)
        if best is None or ll > best[0]:
            best = (ll, g)
    return best


if __name__ == "__main__" and "--ber" in sys.argv:
    ber_main(sys.argv[1:])
