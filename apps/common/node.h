/*
 * Common node skeleton (BR and RN). node.c owns the GENERICITY_PLAN.md §3.3
 * init sequence and the application tasklet; the role files (apps/br/br.c,
 * apps/rn/rn.c) provide the hooks below.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_NODE_H
#define APP_NODE_H

#include <stdint.h>
#include <stdbool.h>

#define APP_EVENT_STATUS 0x40   /* periodic status print */
#define APP_EVENT_PING   0x41   /* next ping of a series */

struct node {
    int8_t ws_if;       /* Wi-SUN interface (ws0) */
    int8_t bh_if;       /* BR backhaul (bh0), -1 on RN */
    int8_t rf_id;
    int8_t tasklet;
    uint8_t eui64[8];
    bool bootstrap_ready;
    uint32_t ready_count;          /* ARM_NWK_BOOTSTRAP_READY seen */
    uint32_t boot_ms;
    uint32_t ready_ms;             /* first BOOTSTRAP_READY, ms since boot */
};
extern struct node g_node;

/* ------- role hooks (one implementation per image) -------- */
extern const char *const app_role_name;          /* "BR" / "RN" */
/* Before the tasklet: create extra interfaces (BR: backhaul). */
void app_role_create_interfaces(void);
/* In the tasklet INIT event, before the common ws configuration:
 * arm_nwk_interface_configure_*_bootstrap_set(). */
void app_role_bootstrap_set(void);
/* After the common ws configuration: credentials, role-specific settings. */
void app_role_configure(void);
/* Bring interfaces up (BR: + ws_bbr_start). */
void app_role_start(void);
/* Extra status lines (BR: routing table summary). */
void app_role_status(bool verbose);
/* Role CLI command, return true if handled. */
bool app_role_cli(int argc, char **argv);

/* ------- common services -------- */
void node_print_status(bool verbose);
void udp_test_init(void);
int udp_test_send(const uint8_t addr[16], const char *text);
void ping_init(void);
int ping_start(const uint8_t addr[16], unsigned count, unsigned size);
void ping_on_timer(void);
void cli_init(void);
void cli_poll(void);

#endif
