#!/usr/bin/env python3
"""Prefix each stdin line with the host time (unix s.ms) and append to a file."""
import sys, time
out = open(sys.argv[1], "ab", buffering=0)
for line in sys.stdin.buffer:
    out.write(b"%.3f " % time.time() + line.rstrip(b"\r\n") + b"\n")
