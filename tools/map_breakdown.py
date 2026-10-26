#!/usr/bin/env python3
"""Per-component flash/RAM breakdown from a GNU ld map file.

usage: tools/map_breakdown.py build/fw/wisun_br.map
Components: stack libraries, MAC/FHSS, mbedTLS, RAIL library, SDK (slc
objects: FreeRTOS, emlib, services...), port, radio driver, app, libc/libgcc.
"""
import collections
import re
import sys


def component(path):
    p = path.replace("\\", "/")
    if "librail_phy.a" in p:
        return "drivers/rail"
    if "librail_" in p:
        return "RAIL library (librail_efr32xg25)"
    if "libnanostack_br_fw.a" in p or "libnanostack_rn_fw.a" in p:
        m = re.search(r"\(([^()]*)\.c\.obj\)", p)
        obj = m.group(1) if m else p
        groups = (
            (("ws_",), "stack: 6lowpan/ws (Wi-SUN)"),
            (("rpl_",), "stack: RPL"),
            (("dhcp", "libdhcpv6"), "stack: DHCPv6"),
            (("icmpv6", "ipv6", "udp", "tcp", "mld"), "stack: IPv6/ICMP/UDP/TCP/MLD"),
            (("auth_", "supp_", "tls_", "eap_", "kmp_", "eapol_", "kde_", "key_sec", "msg_sec", "sec_prot",
              "radius_", "avp_", "pana_", "hmac_", "nist_", "shalib", "ns_sha256"), "stack: security (EAP-TLS, 4WH, GKH, KMP)"),
            (("mpl",), "stack: MPL"),
            (("adaptation_", "lowpan_", "6lowpan_", "iphc_", "cipv6_", "protocol_6lowpan", "network_lib",
              "nd_router", "mac_helper", "mac_ie_lib", "mac_response"), "stack: 6LoWPAN (adaptation, IPHC, ND)"),
        )
        for prefixes, name in groups:
            if obj.startswith(prefixes):
                return name
        return "stack: other (core, net_lib, service_libs, nwk_interface)"
    if "libnanostack_rcp_fw.a" in p:
        return "MAC + FHSS + CCM (rcp/)"
    if "libnanostack_eventloop.a" in p or "libnanostack_libservice.a" in p:
        return "event loop + libservice (incl. ns_trace)"
    if "libmbedtls_fw.a" in p:
        return "mbedTLS 3.0.0"
    if "libns_port_freertos.a" in p:
        return "port/freertos"
    if "librail_phy.a" in p:
        return "drivers/rail"
    if "third_party/freertos" in p:
        return "SDK: FreeRTOS kernel + CMSIS-RTOS2 (incl. heap_4 ucHeap)"
    if "slc.dir" in p or "simplicity_sdk" in p:
        return "SDK: platform (startup, emlib, services, SE, iostream, PA, rail_config)"
    if "libns_port_freertos.a" in p:
        return "port/freertos"
    if "librail_phy.a" in p:
        return "drivers/rail"
    if "wisun_" in p and ".dir/apps/" in p:
        return "apps (node, CLI, tests, certificates)"
    if "libc" in p or "libgcc" in p or "libm" in p or "libnosys" in p:
        return "newlib (full libc) / libgcc"
    return "other"


def main():
    text = open(sys.argv[1]).read().split("Linker script and memory map", 1)[1]
    pat = re.compile(r"^ ([.A-Za-z_][^\s]*)\s*\n?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", re.M)
    agg = collections.defaultdict(lambda: [0, 0])
    for m in pat.finditer(text):
        sec, addr, size, f = m.groups()
        addr, size = int(addr, 16), int(size, 16)
        if not addr or not size:
            continue
        # Classify by address (SDK code-classification sections have custom
        # names such as text_freertos_kernel_timecritical...).
        if 0x08000000 <= addr < 0x08200000:
            kind = 0
        elif 0x20000000 <= addr < 0x20080000:
            kind = 1
        else:
            continue
        agg[component(f)][kind] += size
    tot = [0, 0]
    print(f"{'component':72s} {'flash kB':>9s} {'RAM kB':>8s}")
    for k, v in sorted(agg.items(), key=lambda x: -x[1][0]):
        print(f"{k:72s} {v[0] / 1024:9.1f} {v[1] / 1024:8.1f}")
        tot[0] += v[0]
        tot[1] += v[1]
    print(f"{'TOTAL (input sections; .data counted as RAM only)':72s} {tot[0] / 1024:9.1f} {tot[1] / 1024:8.1f}")


if __name__ == "__main__":
    main()
