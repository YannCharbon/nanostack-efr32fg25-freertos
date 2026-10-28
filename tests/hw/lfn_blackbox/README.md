# LFN black-box test tools

Scripts for the black-box test of Silicon Labs' FAN 1.1 LFN on the two kits (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §8, results in `HW5_BlackBox_Phase1_Results.md` §3). They drive Silicon Labs' own CLI samples (`wisun_soc_lfn_cli`, `wisun_soc_brcli`), not this firmware.

| Script | What it does |
| --- | --- |
| `session.sh` | Opens both kits' consoles and the packet-trace capture for one run |
| `s1_run.py`, `s1_stats.py` | S1, cold join: power-cycles the LFN, joins, times each step |
| `s3_run.py` | S3, pings both ways |
| `aem_loop.sh`, `aem_events.py`, `s2_analyse.py` | Back-to-back AEM dumps at 10 kHz, wake detection, idle summary |
| `dch_cap.py`, `pti_decode.py`, `timeline.py` | Packet trace (PTI) over USB through Commander's `silink`, decoded to frame types and timing IEs, both kits merged on host time |
| `rtt.sh`, `ts.py` | RTT console attempt, time-stamping helper |

Tools are looked up in the root repo's `.deps/` (override with `DEPS`, `SILINK`); `AEM_SN` selects the kit whose AEM is read (default 440235509, the kit with a working AEM). `silink` must be closed on a kit while Commander uses it.
