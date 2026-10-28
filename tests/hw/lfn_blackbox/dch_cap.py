#!/usr/bin/env python3
"""dch_cap.py <port> <outfile>: save the raw silink DATA_CHANNEL stream (DCH frames: PTI/AEM).
Also writes <outfile>.idx: one line "<host unix time> <bytes so far>" per received chunk, so the
adapter's DCH clock can be mapped to host time (lower envelope of host - DCH time)."""
import socket
import sys
import time

import os
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
n = os.path.getsize(sys.argv[2]) if os.path.exists(sys.argv[2]) else 0   # appending: keep offsets global
with open(sys.argv[2], "ab", buffering=0) as f, open(sys.argv[2] + ".idx", "a", buffering=1) as ix:
    while True:
        d = s.recv(65536)
        if not d:
            break
        f.write(d)
        n += len(d)
        ix.write(f"{time.time():.6f} {n}\n")
