/*
 * Compile-time configuration shared by the BR and RN images. Mirrors the
 * emulator's examples/wsbrd.conf / wsnode.conf, except the PHY: NA / class 1 /
 * mode 1b, which is what the generated rail_config.c (BRD4270B, 902-928 MHz)
 * implements. Any value can be overridden with -D at configure time, e.g.
 *   cmake --preset fw -DAPP_EXTRA_DEFS="APP_NETWORK_NAME=\"lab\""
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "nanostack/ws_management_api.h"

#ifndef APP_NETWORK_NAME
#define APP_NETWORK_NAME "Wi-SUN Network"
#endif

/* Must match the radio configuration (sdk/gen/<board>[-mode3]/autogen/rail_config.c),
 * selected by the build's FW_REGION and FW_PHY_MODE:
 *   EU, 1a: domain EU, operating class 1 (863.1 MHz + n*100 kHz, 69 channels),
 *       PHY mode 1a (50 kbit/s 2-FSK, h = 0.5)  - BRD4271A (default)
 *   EU, 3: domain EU, operating class 2 (863.1 MHz + n*200 kHz, 35 channels),
 *       PHY mode 3 (150 kbit/s 2-FSK, h = 0.5)  - BRD4271A, FW_PHY_MODE=3
 *   NA: domain NA, operating class 1 (902.2 MHz + n*200 kHz, 129 channels),
 *       PHY mode 1b (50 kbit/s 2-FSK, h = 1.0)  - BRD4270B */
#if defined FW_REGION_EU && defined FW_PHY_MODE_3
#define APP_WS_DOMAIN_DEFAULT REG_DOMAIN_EU
#define APP_WS_CLASS_DEFAULT  2
#define APP_WS_MODE_DEFAULT   0x03
#define APP_WS_CHAN_LAST_DEFAULT 34
#elif defined FW_REGION_EU
#define APP_WS_DOMAIN_DEFAULT REG_DOMAIN_EU
#define APP_WS_CLASS_DEFAULT  1
#define APP_WS_MODE_DEFAULT   0x1a
#define APP_WS_CHAN_LAST_DEFAULT 68
#else
#define APP_WS_DOMAIN_DEFAULT REG_DOMAIN_NA
#define APP_WS_CLASS_DEFAULT  1
#define APP_WS_MODE_DEFAULT   0x1b
#define APP_WS_CHAN_LAST_DEFAULT 128
#endif
#ifndef APP_WS_DOMAIN
#define APP_WS_DOMAIN APP_WS_DOMAIN_DEFAULT
#endif
#ifndef APP_WS_CLASS
#define APP_WS_CLASS APP_WS_CLASS_DEFAULT
#endif
#ifndef APP_WS_MODE
#define APP_WS_MODE APP_WS_MODE_DEFAULT
#endif

/* Allowed channel range (Wi-SUN channel numbers, inclusive). Default: the
 * whole NA/1b plan, 0..128. Asynchronous frames (PAN advertisement /
 * configuration and their solicits) are swept over every allowed channel:
 * with 129 channels at 50 kbit/s one sweep keeps the radio busy ~2-3 s, during
 * which the node neither receives on its unicast channel nor sends unicast.
 * A narrower range would shorten the sweeps, BUT: EXPERIMENTAL, NOT WORKING
 * YET: in the host twin, with 0..15 the RN selects the BR as parent but its
 * unicast EAPOL frames are never acknowledged (the mask only reaches the
 * unicast schedule, "channels: BC 129 UC 16"); not investigated further.
 * Also, FCC 15.247 requires >= 50 hopping channels in 902-928 MHz. */
#ifndef APP_WS_CHAN_FIRST
#define APP_WS_CHAN_FIRST 0
#endif
#ifndef APP_WS_CHAN_LAST
#define APP_WS_CHAN_LAST APP_WS_CHAN_LAST_DEFAULT
#endif

#ifndef APP_WS_SIZE
#define APP_WS_SIZE NETWORK_SIZE_SMALL
#endif

/* Device minimum sensitivity, as in wsbrd/wsnode: 174 - 93 -> -93 dBm. */
#ifndef APP_DEVICE_MIN_SENS
#define APP_DEVICE_MIN_SENS (174 - 93)
#endif

/* Border router only: static prefix of the (null) backhaul. */
#define APP_BR_PREFIX { 0xfd, 0x12, 0x34, 0x56, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

/* Border router only: force all four GTKs to zero, as examples/wsbrd.conf,
 * so a sniffer can decrypt the traffic. TEST ONLY. */
#ifndef APP_BR_FORCE_ZERO_GTKS
#define APP_BR_FORCE_ZERO_GTKS 1
#endif

/* Nanostack TX power setting (percent of max; the driver maps or fixes it). */
#ifndef APP_TX_POWER_PERCENT
#define APP_TX_POWER_PERCENT 20
#endif

/* MAC tables. The device table also sizes the Wi-SUN neighbour, ETX and MAC
 * neighbour tables (GENERICITY_PLAN.md §2.2 point 2); the emulator RCP uses 255. */
#ifndef APP_MAC_DEVICE_TABLE_SIZE
#define APP_MAC_DEVICE_TABLE_SIZE 32
#endif

/* UDP test port (same as the emulator's multicast helper). */
#ifndef APP_UDP_PORT
#define APP_UDP_PORT 1234
#endif

/* Periodic one-line status on the console, seconds (0 = off). */
#ifndef APP_STATUS_PERIOD_S
#define APP_STATUS_PERIOD_S 30
#endif

/* Stack task. 8 kB: mbedTLS ECP/TLS run in it (measure with `tasks`). */
#ifndef APP_STACK_TASK_STACK_BYTES
#define APP_STACK_TASK_STACK_BYTES 8192
#endif

#endif /* APP_CONFIG_H */
