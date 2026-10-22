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

/* Must match the radio configuration (sdk/gen/autogen/rail_config.c). */
#ifndef APP_WS_DOMAIN
#define APP_WS_DOMAIN REG_DOMAIN_NA
#endif
#ifndef APP_WS_CLASS
#define APP_WS_CLASS 1
#endif
#ifndef APP_WS_MODE
#define APP_WS_MODE 0x1b
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
