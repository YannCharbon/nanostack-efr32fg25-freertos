/*
 * Wi-SUN Router Node role (NSCONFIG=ws_router): EAP-TLS supplicant, DHCPv6
 * client, RPL router. Same steps as orch/app_wsbrd/wsrouter.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "mbed-client-libservice/ns_trace.h"
#include "nanostack/net_interface.h"

#include "ns_port.h"
#include "app_config.h"
#include "certs.h"
#include "node.h"

#define TRACE_GROUP "rn"

const char *const app_role_name = "RN";

static arm_certificate_entry_s tls_own;
static arm_certificate_entry_s tls_ca;

void app_role_create_interfaces(void)
{
}

void app_role_bootstrap_set(void)
{
    int ret = arm_nwk_interface_configure_6lowpan_bootstrap_set(g_node.ws_if, NET_6LOWPAN_ROUTER, NET_6LOWPAN_WS);
    if (ret) tr_warn("6lowpan bootstrap set: %d", ret);
}

void app_role_configure(void)
{
    int ret;

    tls_own.cert = (const uint8_t *)cert_node_cert_pem;
    tls_own.cert_len = cert_node_cert_pem_len;
    tls_own.key = (const uint8_t *)cert_node_key_pem;
    tls_own.key_len = cert_node_key_pem_len;
    tls_ca.cert = (const uint8_t *)cert_ca_cert_pem;
    tls_ca.cert_len = cert_ca_cert_pem_len;
    ret = arm_network_own_certificate_add(&tls_own);
    if (ret) tr_warn("own certificate: %d", ret);
    ret = arm_network_trusted_certificate_add(&tls_ca);
    if (ret) tr_warn("trusted certificate: %d", ret);
}

void app_role_start(void)
{
    if (arm_nwk_interface_up(g_node.ws_if)) {
        tr_warn("arm_nwk_interface_up ws0");
    }
}

void app_role_status(bool verbose)
{
    (void)verbose;
}

bool app_role_cli(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return false;
}
