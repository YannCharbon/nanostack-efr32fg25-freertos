# LFN black-box test tools

Scripts for the black-box test of Silicon Labs' FAN 1.1 LFN on the two kits (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §8, results in `HW5_BlackBox_Phase1_Results.md` §3 and §5). They drive Silicon Labs' own programs (`wisun_soc_lfn_cli`, `wisun_soc_brcli`, `wsbrd` + `wisun_rcp`), not this firmware.

| Script | What it does |
| --- | --- |
| `session.sh` | Opens both kits' consoles and the packet-trace capture for one run (embedded border router as parent) |
| `s1_run.py`, `s1_stats.py` | S1, cold join with the embedded border router: power-cycles the LFN, joins, times each step; per-step statistics (also for `s1w_run.py`) |
| `s3_run.py` | S3, pings both ways through the CLIs |
| `aem_loop.sh`, `aem_events.py`, `s2_analyse.py` | Back-to-back AEM dumps at 10 kHz, wake detection, idle summary |
| `dch_cap.py`, `pti.py`, `pti_decode.py`, `timeline.py` | Packet trace (PTI) over USB through Commander's `silink` (`pti.py start/stop SN FILE`), decoded to frame types and timing IEs, or written as an 802.15.4 pcap (`--pcap154`) for tshark; both kits, the consoles and `wsbrd`'s trace (`--wsbrd`) merged on host time |
| `wsbrd.py`, `wsbrd_p1.conf.in` | Start/stop `wsbrd` (kit B runs `wisun_rcp`) and the LFN console; one pcap per `wsbrd` start |
| `s1w_run.py` | S1 with `wsbrd` as parent (new PAN each repetition) |
| `s4w_run.py` | S4, pings from the host through `wsbrd`'s TUN to multicast groups, every reply logged |
| `s5w_run.py` | S5, `wsbrd` stopped and restarted with its storage kept; time to the LFN's loss and rejoin |
| `s6_lgtk_fast.conf`, `s5_sync1.conf`, `s5_sync2.conf` | `wsbrd` settings appended with `--extra`: LGTK rotation in minutes, LFN sync period 1 or 2 |
| `tshark_keys.py` | Wireshark key table (GAKs from the (L)GTKs and the network name), so tshark decrypts the PTI pcaps |
| `lfn_cli_test_profiles.patch` | Change to the `wisun_soc_lfn_cli` sample (generated in `.deps/lfn_build/`): extra profiles `test_short` (Test, registration lifetime 5 min), `eco_short` (Eco, unicast 30 s, short DHCP delay, lifetime 10 min), `test_mpt20` (Test, `lfn_maintain_parent_time` 20) |
| `rtt.sh`, `ts.py` | RTT console attempt, time-stamping helper |

Tools are looked up in the root repo's `.deps/` (override with `DEPS`, `SILINK`, `WSBRD`); `AEM_SN` selects the kit whose AEM is read (default 440235509, the kit with a working AEM). `silink` must be closed on a kit while Commander uses it.

**`wsbrd` setup** (`wisun-br-linux` v2.10.3 in `.deps/`):
- The RCP must be the **UART** variant of `wisun_rcp`: the SDK's default project uses CPC, which `wsbrd`'s `uart_device` does not speak. Generate it with `slc generate ... --with brd4271a,brd4001a,wisun_rcp_uart --without wisun_rcp_cpc` (`.deps/lfn_build/wisun_rcp_uart/`, 115200 baud, no flow control); kit B's VCOM: `commander vcom config -s 440240046 --baudrate 115200 --handshake disable`.
- Without root, `wsbrd` needs `cap_net_admin,cap_net_raw,cap_net_bind_service` on its binary (TUN, raw ICMPv6, DHCPv6 server on port 547). With file capabilities, its D-Bus interface does not come up (sd-bus ignores the session-bus environment of a privileged binary, and the system bus refuses the name), so D-Bus methods (`JoinMulticastGroup`, `RevokeLgtks`, ...) are not available; key rotation is driven by the timers in its config instead.
- Its TUN is `tunlfn` with prefix `fd00:5eed::/64` (the emulator's `tun0`/`tunvt*` hold `fd12:3456::/64` on this PC).

**tshark** (no root): Ubuntu's `tshark` and the libraries missing on this PC, fetched with `apt-get download` and unpacked with `dpkg -x` into `.deps/tshark/root/`; run it through `.deps/tshark/tshark.sh` (sets the library path and `WIRESHARK_CONFIG_DIR=.deps/tshark/config`, where `tshark_keys.py` writes the keys).
