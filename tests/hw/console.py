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
Exit status: 0 when stopped (SIGTERM, SIGINT), 1 if the tty cannot be opened
or goes away (kit unplugged or re-enumerated).
"""
import os
import select
import signal
import sys
import time

import serial


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
    mode = "ab" if os.environ.get("CONSOLE_APPEND") == "1" else "wb"
    with open(log, mode, buffering=0) as out:
        out.write(b"%.3f === console %s on %s\n" % (time.time(), mode.encode(), tty.encode()))
        while True:
            rl = [ser.fileno()] + ([fd_fifo] if fd_fifo is not None else [])
            r, _, _ = select.select(rl, [], [], 1.0)
            if ser.fileno() in r:
                try:
                    data = ser.read(4096)
                except (serial.SerialException, OSError) as e:
                    out.write(b"%.3f === console lost: %s\n" % (time.time(), str(e).encode()))
                    return 1
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    out.write(b"%.3f " % time.time() + line.rstrip(b"\r") + b"\n")
            if fd_fifo is not None and fd_fifo in r:
                data = os.read(fd_fifo, 4096)
                for cmd in data.decode(errors="ignore").splitlines():
                    if cmd.strip():
                        out.write(b"%.3f >>> %s\n" % (time.time(), cmd.encode()))
                        for ch in (cmd + "\r").encode():
                            ser.write(bytes([ch]))
                            time.sleep(0.002)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
