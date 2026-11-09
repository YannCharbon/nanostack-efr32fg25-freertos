#!/usr/bin/env python3
"""Minimal driver for Silicon Labs' RAILtest CLI on the FG25 kits.

RAILtest answers each command with lines of the form
"{{(cmd)}{key:value}...}"; asynchronous events (received packets, TX end)
use the same format. RT.cmd() sends one line and returns the parsed
responses that arrive within `wait` seconds; RT.drain() collects events.
Every line in and out is appended to a log file (raw record).
"""
import re
import sys
import time

import serial

KIT_TTY = "/dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_000{}-if00"
ITEM = re.compile(r"\{([^{}:]+):([^{}]*)\}")
HEAD = re.compile(r"^(?:> )?\{\{\(([^)]*)\)\}")


def parse(line):
    """'{{(rx)}{len:20}{rssi:-40}...}' -> ('rx', {'len': '20', 'rssi': '-40'})"""
    m = HEAD.match(line)
    if not m:
        return None, {}
    return m.group(1), dict(ITEM.findall(line[m.end() - 1:]))


class RT:
    def __init__(self, sn, log=None, name=None):
        self.sn = str(sn)
        self.name = name or self.sn
        self.ser = serial.Serial(KIT_TTY.format(self.sn), 115200, timeout=0.002)
        self.buf = b""
        self.log = open(log, "a") if log else None

    def _log(self, d, s):
        if self.log:
            self.log.write(f"{time.time():.3f} {self.name} {d} {s}\n")

    def lines(self, wait):
        """Lines received within `wait` s (returns as soon as `wait` elapses)."""
        out, t_end = [], time.time() + wait
        while True:
            self.buf += self.ser.read(max(1, self.ser.in_waiting))
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                s = l.decode(errors="replace").strip()
                if s:
                    self._log("<", s)
                    out.append(s)
            if time.time() >= t_end:
                return out

    def send(self, line):
        self._log(">", line)
        self.ser.write(line.encode() + b"\r\n")

    def cmd(self, line, wait=0.3, until=None):
        """Send `line`; return parsed responses (list of (name, dict)).
        Returns early once a response named `until` (default: the command's
        own name) has been seen."""
        self.send(line)
        name = until or line.split()[0]
        out, t_end = [], time.time() + wait
        while time.time() < t_end:
            for l in self.lines(0.02):
                out.append(parse(l))
            if any(n == name for n, _ in out):
                # take whatever trails in the same burst
                out += [parse(l) for l in self.lines(0.02)]
                break
        return out

    def get(self, line, key, wait=1.0):
        for n, d in self.cmd(line, wait):
            if key in d:
                return d[key]
        return None


if __name__ == "__main__":
    r = RT(sys.argv[1])
    for c in sys.argv[2:]:
        for n, d in r.cmd(c, 1.0):
            print(n, d)
