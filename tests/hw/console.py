#!/usr/bin/env python3
"""Log a kit's VCOM to a file and forward commands from a FIFO.

usage: tests/hw/console.py <tty> <logfile> [cmd_fifo]
Each received line is written as "<host unix time.ms> <line>". Lines written
to cmd_fifo are sent to the board followed by CR.
"""
import os
import select
import sys
import time

import serial


def main():
    tty, log = sys.argv[1], sys.argv[2]
    fifo = sys.argv[3] if len(sys.argv) > 3 else None
    ser = serial.Serial(tty, 115200, timeout=0)
    fd_fifo = None
    if fifo:
        if not os.path.exists(fifo):
            os.mkfifo(fifo)
        fd_fifo = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    buf = b""
    with open(log, "ab", buffering=0) as out:
        while True:
            rl = [ser.fileno()] + ([fd_fifo] if fd_fifo is not None else [])
            r, _, _ = select.select(rl, [], [], 1.0)
            if ser.fileno() in r:
                buf += ser.read(4096)
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
    main()
