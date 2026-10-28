#!/usr/bin/env python3
"""Write Wireshark's 802.15.4 key table for a Wi-SUN network, so that tshark decrypts frames
captured over PTI (wsbrd's own pcap is already unencrypted).

usage: tshark_keys.py CONFIG_DIR NETNAME [--gtk I:HEX ...] [--lgtk I:HEX ...] [--storage network-keys ...]
GAK = Truncate-128(SHA-256(network name || (L)GTK)) (FAN 1.1 6.5.4.1.1); MAC key index =
GTK slot + 1 (1-4), LGTK slot + 5 (5-7). Use with WIRESHARK_CONFIG_DIR=CONFIG_DIR.
HEX may contain colons. Slots and keys are in wsbrd's storage file (keys-*) or its config.
"""
import argparse
import hashlib
import os

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("netname")
ap.add_argument("--gtk", action="append", default=[])
ap.add_argument("--lgtk", action="append", default=[])
ap.add_argument("--storage", action="append", default=[], help="wsbrd storage file network-keys (all its keys)")
a = ap.parse_args()
import re
for f in a.storage:
    for m in re.finditer(r"^(l?gtk)\[(\d)\] = ([0-9a-f:]{47})$", open(f).read(), re.M):
        (a.lgtk if m.group(1) == "lgtk" else a.gtk).append(f"{m.group(2)}:{m.group(3)}")
os.makedirs(a.dir, exist_ok=True)
lines, seen = [], set()
for lst, base in ((a.gtk, 1), (a.lgtk, 5)):
    for item in lst:
        slot, key = item.split(":", 1)
        gtk = bytes.fromhex(key.replace(":", ""))
        gak = hashlib.sha256(a.netname.encode() + gtk).digest()[:16]
        if (slot, key, base) in seen:
            continue
        seen.add((slot, key, base))
        lines.append(f'"{gak.hex()}","{int(slot) + base}","No hash"')
open(os.path.join(a.dir, "ieee802154_keys"), "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
