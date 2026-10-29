/*
 * Wi-SUN Border Router role (NSCONFIG=ws_border_router).
 *
 * Runs the authenticator (EAP-TLS server, local, no RADIUS), the DHCPv6
 * server and the RPL root, all selected by NET_6LOWPAN_BORDER_ROUTER +
 * ws_bbr_start(). There is no real backhaul: a null "TUN-type" driver backs
 * the bh0 interface, which only carries the static prefix fd12:3456::/64
 * that the BBR advertises (ws_bbr_bb_static_prefix_get() reads it from the
 * backbone interface, GENERICITY_PLAN.md §5.6). Without any backbone
 * interface the BBR would fall back to a backup ULA only after 300 s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/ip6string.h"
#include "nanostack/net_interface.h"
#include "nanostack/ethernet_mac_api.h"
#include "nanostack/ws_bbr_api.h"
#include "nanostack/ws_management_api.h"
#include "nanostack/net_ws_test.h"
#include "nanostack/mac/platform/arm_hal_phy.h"

#include "ns_port.h"
#include "app_config.h"
#include "certs.h"
#include "node.h"

#define TRACE_GROUP "br"

const char *const app_role_name = "BR";

/* ---------------------------------------------------- null backhaul (bh0) */

static uint8_t bh_mac[8] = { 20, 21, 22, 23, 24, 25, 26, 27 };   /* as orch/app_wsbrd/tun.c */
static int8_t bh_driver_id = -1;
static uint32_t bh_tx_dropped;

static int8_t bh_tx(uint8_t *buf, uint16_t len, uint8_t handle, data_protocol_e proto);

static phy_device_driver_s bh_driver = {
    .link_type = PHY_LINK_TUN,
    .data_request_layer = IPV6_DATAGRAMS_DATA_FLOW,
    .PHY_MAC = bh_mac,
    .driver_description = (char *)"NULL BH",
    .tx = bh_tx,
};

static int8_t bh_tx(uint8_t *buf, uint16_t len, uint8_t handle, data_protocol_e proto)
{
    (void)buf;
    (void)len;
    (void)proto;
    /* Nowhere to send it: drop, but complete the TX so the stack moves on. */
    bh_tx_dropped++;
    bh_driver.phy_tx_done_cb(bh_driver_id, handle, PHY_LINK_TX_SUCCESS, 0, 0);
    return 0;
}

static arm_certificate_entry_s tls_own;
static arm_certificate_entry_s tls_ca;

void app_role_create_interfaces(void)
{
    eth_mac_api_t *bh_mac_api;

    bh_driver_id = arm_net_phy_register(&bh_driver);
    configASSERT(bh_driver_id >= 0);
    bh_mac_api = ethernet_mac_create(bh_driver_id);
    configASSERT(bh_mac_api);
    g_node.bh_if = arm_nwk_interface_ethernet_init(bh_mac_api, "bh0");
    configASSERT(g_node.bh_if >= 0);
}

void app_role_bootstrap_set(void)
{
    static const uint8_t prefix[16] = APP_BR_PREFIX;
    int ret;

    ret = arm_nwk_interface_configure_6lowpan_bootstrap_set(g_node.ws_if, NET_6LOWPAN_BORDER_ROUTER, NET_6LOWPAN_WS);
    if (ret) tr_warn("6lowpan bootstrap set: %d", ret);
    ret = arm_nwk_interface_configure_ipv6_bootstrap_set(g_node.bh_if, NET_IPV6_BOOTSTRAP_STATIC, prefix);
    if (ret) tr_warn("ipv6 bootstrap set: %d", ret);
}

void app_role_configure(void)
{
    int ret;

    tls_own.cert = (const uint8_t *)cert_br_cert_pem;
    tls_own.cert_len = cert_br_cert_pem_len;
    tls_own.key = (const uint8_t *)cert_br_key_pem;
    tls_own.key_len = cert_br_key_pem_len;
    tls_ca.cert = (const uint8_t *)cert_ca_cert_pem;
    tls_ca.cert_len = cert_ca_cert_pem_len;
    ret = arm_network_own_certificate_add(&tls_own);
    if (ret) tr_warn("own certificate: %d", ret);
    ret = arm_network_trusted_certificate_add(&tls_ca);
    if (ret) tr_warn("trusted certificate: %d", ret);
#ifdef APP_LFN_PARENT
    /* FAN 1.1 with LFN parenting (GS8 stage 6, FW_LFN_PARENT): the LFN
       settings' defaults (the stack's docs/LFN_STACK.md §5) */
    {
        ws_lfn_settings_t lfn;
        ret = ws_management_fan_version_set(g_node.ws_if, 2 /* FAN 1.1 (1: FAN 1.0) */);
        if (ret) tr_warn("ws_management_fan_version_set: %d", ret);
        ws_management_lfn_settings_defaults(&lfn);
        ret = ws_management_lfn_settings_set(g_node.ws_if, &lfn);
        if (ret) tr_warn("ws_management_lfn_settings_set: %d", ret);
        ret = ws_management_lfn_parent_set(g_node.ws_if, true);
        if (ret) tr_warn("ws_management_lfn_parent_set: %d", ret);
        tr_info("FAN 1.1, LFN parenting on");
    }
#endif
#if APP_BR_FORCE_ZERO_GTKS
    {
        static uint8_t zero_gtk[4][16];
        uint8_t *gtks[4] = { zero_gtk[0], zero_gtk[1], zero_gtk[2], zero_gtk[3] };
        ret = ws_test_gtk_set(g_node.ws_if, gtks);
        if (ret) tr_warn("ws_test_gtk_set: %d", ret);
        tr_warn("TEST MODE: all GTKs forced to zero (sniffer-decryptable)");
    }
#endif
}

void app_role_start(void)
{
    if (arm_nwk_interface_up(g_node.bh_if)) {
        tr_warn("arm_nwk_interface_up bh0");
    }
    if (arm_nwk_interface_up(g_node.ws_if)) {
        tr_warn("arm_nwk_interface_up ws0");
    }
    if (ws_bbr_start(g_node.ws_if, g_node.bh_if)) {
        tr_warn("ws_bbr_start");
    } else {
        tr_info("BBR started");
    }
}

static void print_nodes(void)
{
    bbr_information_t info;
    bbr_route_info_t *table;
    int n;
    char a[48];

    if (ws_bbr_info_get(g_node.ws_if, &info)) {
        ns_port_console_printf("   bbr: no info\r\n");
        return;
    }
    ip6tos(info.dodag_id, a);
    ns_port_console_printf("   bbr: dodag_id %s, %u devices, instance %u version %u\r\n", a,
                           info.devices_in_network, info.instance_id, info.version);
    if (!info.devices_in_network) {
        return;
    }
    table = malloc(sizeof(*table) * info.devices_in_network);
    if (!table) {
        return;
    }
    n = ws_bbr_routing_table_get(g_node.ws_if, table, info.devices_in_network);
    for (int i = 0; i < n; i++) {
        uint8_t t[16], p[16];
        char ta[48], pa[48];
        memcpy(t, info.prefix, 8);
        memcpy(t + 8, table[i].target, 8);
        memcpy(p, info.prefix, 8);
        memcpy(p + 8, table[i].parent, 8);
        ip6tos(t, ta);
        ip6tos(p, pa);
        ns_port_console_printf("   node %s parent %s\r\n", ta, pa);
    }
    free(table);
}

void app_role_status(bool verbose)
{
    int n = 0;
    uint8_t addr[16];
    char a[48];

    while (arm_net_address_list_get_next(g_node.bh_if, &n, addr) >= 0) {
        ip6tos(addr, a);
        ns_port_console_printf("   bh0 addr %s\r\n", a);
    }
    print_nodes();
    if (verbose) {
        ns_port_console_printf("   bh0 null backhaul: %lu packets dropped\r\n", (unsigned long)bh_tx_dropped);
    }
}

bool app_role_cli(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[0], "nodes")) {
        print_nodes();
        return true;
    }
    return false;
}
