#!/usr/bin/env python3
"""Log a kit's VCOM to a file and forward commands from a FIFO.

usage: tests/hw/console.py <tty> <logfile> [cmd_fifo]
Each received line is written as "<host unix time.ms> <line>". Lines written
to cmd_fifo are sent to the board followed by CR. The log starts empty
(a log from an earlier run would satisfy the next run's searches);
CONSOLE_APPEND=1 appends instead (callers that search from a recorded
offset, such as lfn_blackbox/p23lib.py).
Environment: CONSOLE_BAUD (default 115200), CONSOLE_RTSCTS=1 for hardware flow
control (Silicon Labs' Wi-SUN sample CLIs), CONSOLE_APPEND.

Command check: once the board has shown our CLI's prompt ("BR> ", "RN> ",
"LFN> ") or our trace or status lines, every command must bring a prompt back within CONFIRM_S; if not,
the CR is sent again (up to CR_RETRIES times), which runs a line whose CR was
lost and is ignored by our CLI on an empty line. Each case is logged
("=== console: ..."). Silicon Labs' CLIs never show our prompt and are left
alone (their "wisun ping" holds the prompt until the reply). Without it, the
kits' border router once lost the CR of 2 of 237 commands under heavy trace
output (weekend run 2026-11-06, Silicon Labs' LFN under our border router):
the command never ran and the test counted a lost downlink.
Exit status: 0 when stopped (SIGTERM, SIGINT), 1 if the tty cannot be opened
or goes away (kit unplugged or re-enumerated).
"""
import os
import re
import select
import signal
import sys
import time

import serial

PROMPT = re.compile(rb"^(?:BR|RN|LFN|N)> ")
# our firmware's trace and status lines (Silicon Labs' CLIs print neither)
OURS = re.compile(rb"^(?:\[\d+\] \[(?:INFO|WARN|ERR |DBG )\]|== (?:BR|RN|LFN|N) )")
CONFIRM_S = 2.0
CR_RETRIES = 2


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    tty, log = sys.argv[1], sys.argv[2]
    fifo = sys.argv[3] if len(sys.argv) > 3 else None
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        ser = serial.Serial(tty, int(os.environ.get("CONSOLE_BAUD", "115200")), timeout=0,
                            rtscts=os.environ.get("CONSOLE_RTSCTS") == "1")
    except (serial.SerialException, OSError) as e:
        print(f"console.py: {tty}: {e}", file=sys.stderr)
        return 1
    fd_fifo = None
    if fifo:
        if not os.path.exists(fifo):
            os.mkfifo(fifo)
        fd_fifo = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    buf = b""
    ours = False          # our CLI's prompt seen on this console
    pend = None           # [deadline, CRs sent again, command] of an unconfirmed command
    rx_total = 0          # bytes received; buf starts at rx_total - len(buf)
    pend_from = 0         # a prompt confirms only if its line starts at or after this offset
    mode = "ab" if os.environ.get("CONSOLE_APPEND") == "1" else "wb"
    with open(log, mode, buffering=0) as out:
        out.write(b"%.3f === console %s on %s\n" % (time.time(), mode.encode(), tty.encode()))
        while True:
            rl = [ser.fileno()] + ([fd_fifo] if fd_fifo is not None else [])
            tmo = 1.0 if pend is None else max(0.0, min(1.0, pend[0] - time.time()))
            r, _, _ = select.select(rl, [], [], tmo)
            if ser.fileno() in r:
                try:
                    data = ser.read(4096)
                except (serial.SerialException, OSError) as e:
                    out.write(b"%.3f === console lost: %s\n" % (time.time(), str(e).encode()))
                    return 1
                buf += data
                rx_total += len(data)
                while b"\n" in buf:
                    start = rx_total - len(buf)
                    line, buf = buf.split(b"\n", 1)
                    if PROMPT.match(line) and start >= pend_from:
                        ours, pend = True, None
                    elif OURS.match(line):
                        ours = True
                    out.write(b"%.3f " % time.time() + line.rstrip(b"\r") + b"\n")
                # a prompt with nothing after it yet (a quiet LFN)
                if PROMPT.match(buf) and rx_total - len(buf) >= pend_from:
                    ours, pend = True, None
            if pend is not None and time.time() >= pend[0]:
                if pend[1] < CR_RETRIES:
                    pend[1] += 1
                    pend[0] = time.time() + CONFIRM_S
                    out.write(b"%.3f === console: no prompt after '%s', CR sent again (%d)\n"
                              % (time.time(), pend[2].encode(), pend[1]))
                    ser.write(b"\r")
                else:
                    out.write(b"%.3f === console: no prompt after '%s' and %d more CRs\n"
                              % (time.time(), pend[2].encode(), pend[1]))
                    pend = None
            if fd_fifo is not None and fd_fifo in r:
                data = os.read(fd_fifo, 4096)
                for cmd in data.decode(errors="ignore").splitlines():
                    if cmd.strip():
                        out.write(b"%.3f >>> %s\n" % (time.time(), cmd.encode()))
                        for ch in (cmd + "\r").encode():
                            ser.write(bytes([ch]))
                            time.sleep(0.002)
                        if ours:
                            pend = [time.time() + CONFIRM_S, 0, cmd]
                            pend_from = rx_total + 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
