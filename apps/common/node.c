/*
 * Node start-up and application tasklet, shared by the BR and RN images.
 *
 * The init sequence is GENERICITY_PLAN.md §3.3 (in-process host <-> MAC):
 *   critical init, trace, radio + timebase, event loop, PHY driver register,
 *   ns_sw_mac_create(), net_init_core(), arm_nwk_interface_lowpan_init(),
 *   [BR: null backhaul], tasklet; then ns_port_run() forever.
 * The tasklet INIT event does what wsbr.c / wsrouter.c wsbr_configure_ws()
 * do in the emulator, with ws_management_node_init(..., &ns_port_fhss_timer)
 * instead of the Spinel proxy's (fhss_timer_t *)-1.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "sl_main_init.h"
#include "sl_power_manager.h"
#include "rail.h"

#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/ip6string.h"
#include "nanostack-event-loop/eventOS_event.h"
#include "nanostack-event-loop/eventOS_scheduler.h"
#include "nanostack-event-loop/eventOS_event_timer.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/sw_mac.h"
#include "nanostack/mac/fhss_config.h"
#include "nanostack/mac/mlme.h"
#include "nanostack/net_interface.h"
#include "nanostack/ws_management_api.h"
#include "6lowpan/ws/ws_common_defines.h"   /* WS_FHSS_*_INTERVAL defaults */
#include "ns_port/critical.h"

#include "ns_port.h"
#include "rail_phy.h"
#include "app_config.h"
#include "node.h"

#define TRACE_GROUP "app"

struct node g_node = { .ws_if = -1, .bh_if = -1, .rf_id = -1, .tasklet = -1 };

static StaticTask_t stack_task_tcb;
static StackType_t stack_task_stack[APP_STACK_TASK_STACK_BYTES / sizeof(StackType_t)];

static const char *const nwk_event_names[] = {
    "ARM_NWK_BOOTSTRAP_READY",
    "ARM_NWK_RPL_INSTANCE_FLOODING_READY",
    "ARM_NWK_SET_DOWN_COMPLETE",
    "ARM_NWK_NWK_SCAN_FAIL",
    "ARM_NWK_IP_ADDRESS_ALLOCATION_FAIL",
    "ARM_NWK_DUPLICATE_ADDRESS_DETECTED",
    "ARM_NWK_AUHTENTICATION_START_FAIL",
    "ARM_NWK_AUHTENTICATION_FAIL",
    "ARM_NWK_NWK_CONNECTION_DOWN",
    "ARM_NWK_NWK_PARENT_POLL_FAIL",
    "ARM_NWK_PHY_CONNECTION_DOWN",
};

/* ms since boot from the RTOS tick (1 kHz): the 32-bit µs RAIL clock wraps
 * every 71 min, the tick count after 49 days. */
static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* Common part of wsbr_configure_ws() (orch/app_wsbrd/wsbr.c, wsrouter.c). */
static void configure_ws(void)
{
    int ret;

    ret = ws_management_node_init(g_node.ws_if, APP_WS_DOMAIN, (char *)APP_NETWORK_NAME,
                                  (fhss_timer_t *)&ns_port_fhss_timer);
    if (ret) tr_warn("ws_management_node_init: %d", ret);
    ret = ws_management_regulatory_domain_set(g_node.ws_if, APP_WS_DOMAIN, APP_WS_CLASS, APP_WS_MODE);
    if (ret) tr_warn("ws_management_regulatory_domain_set: %d", ret);
    ret = ws_management_fhss_unicast_channel_function_configure(g_node.ws_if, WS_DH1CF, 0xffff,
                                                                WS_FHSS_UC_DWELL_INTERVAL);
    if (ret) tr_warn("uc channel function: %d", ret);
    ret = ws_management_fhss_broadcast_channel_function_configure(g_node.ws_if, WS_DH1CF, 0xffff,
                                                                  WS_FHSS_BC_DWELL_INTERVAL, WS_FHSS_BC_INTERVAL);
    if (ret) tr_warn("bc channel function: %d", ret);
    if (APP_WS_CHAN_FIRST != 0 || APP_WS_CHAN_LAST != APP_WS_CHAN_LAST_DEFAULT) {
        uint32_t mask[8] = { 0 };
        for (unsigned ch = APP_WS_CHAN_FIRST; ch <= APP_WS_CHAN_LAST && ch < 256; ch++) {
            mask[ch / 32] |= 1u << (ch % 32);
        }
        ret = ws_management_channel_mask_set(g_node.ws_if, mask);
        if (ret) tr_warn("ws_management_channel_mask_set: %d", ret);
        tr_info("allowed channels %u..%u", APP_WS_CHAN_FIRST, APP_WS_CHAN_LAST);
    }
    ret = ws_management_network_size_set(g_node.ws_if, APP_WS_SIZE);
    if (ret) tr_warn("ws_management_network_size_set: %d", ret);
    ret = ws_device_min_sens_set(g_node.ws_if, APP_DEVICE_MIN_SENS);
    if (ret) tr_warn("ws_device_min_sens_set: %d", ret);
    ret = arm_nwk_set_tx_output_power(g_node.ws_if, APP_TX_POWER_PERCENT);
    if (ret) tr_warn("arm_nwk_set_tx_output_power: %d", ret);
}

static void app_tasklet(arm_event_t *ev)
{
    switch (ev->event_type) {
        case ARM_LIB_TASKLET_INIT_EVENT:
            g_node.tasklet = ev->receiver;
            /* The tasklet calling *_bootstrap_set() receives ARM_LIB_NWK_INTERFACE_EVENT. */
            app_role_bootstrap_set();
            configure_ws();
            app_role_configure();
            udp_test_init();
            ping_init();
            app_role_start();
            tr_info("%s: interfaces up, network \"%s\", domain %u class %u mode 0x%02x",
                    app_role_name, APP_NETWORK_NAME, APP_WS_DOMAIN, APP_WS_CLASS, APP_WS_MODE);
            if (APP_STATUS_PERIOD_S) {
                eventOS_event_timer_request(0, APP_EVENT_STATUS, g_node.tasklet, APP_STATUS_PERIOD_S * 1000u);
            }
            break;
        case ARM_LIB_NWK_INTERFACE_EVENT: {
            const char *name = ev->event_data < sizeof(nwk_event_names) / sizeof(nwk_event_names[0])
                               ? nwk_event_names[ev->event_data] : "?";
            tr_info("network event on if %d: %s", ev->event_id, name);
            if (ev->event_id == g_node.ws_if) {
                if (ev->event_data == ARM_NWK_BOOTSTRAP_READY) {
                    g_node.bootstrap_ready = true;
                    g_node.ready_count++;
                    if (!g_node.ready_ms) {
                        g_node.ready_ms = now_ms() - g_node.boot_ms;
                    }
                    node_print_status(false);
#ifdef APP_ROLE_LFN
                    if (APP_LFN_QUIET_AFTER_S) {
                        eventOS_event_timer_request(0, APP_EVENT_QUIET, g_node.tasklet, APP_LFN_QUIET_AFTER_S * 1000u);
                    }
#endif
                } else {
                    g_node.bootstrap_ready = false;
                }
            }
            break;
        }
        case APP_EVENT_QUIET:
            tr_warn("LFN joined %u s ago: traces at ERROR from now (APP_LFN_QUIET_AFTER_S)", APP_LFN_QUIET_AFTER_S);
            mbed_trace_config_set(TRACE_ACTIVE_LEVEL_ERROR);
            break;
        case APP_EVENT_STATUS:
            node_print_status(false);
            eventOS_event_timer_request(0, APP_EVENT_STATUS, g_node.tasklet, APP_STATUS_PERIOD_S * 1000u);
            break;
        case APP_EVENT_PING:
            ping_on_timer();
            break;
        default:
            break;
    }
}

void node_lfn_parent_configure(void)
{
    ws_lfn_settings_t lfn;
    int ret;

    /* The LFN settings' defaults (the stack's docs/LFN_STACK.md §5). A router
     * parents LFNs once it is operational and holds an LGTK (§9). */
    ret = ws_management_fan_version_set(g_node.ws_if, 2 /* FAN 1.1 (1: FAN 1.0) */);
    if (ret) tr_warn("ws_management_fan_version_set: %d", ret);
    ws_management_lfn_settings_defaults(&lfn);
    ret = ws_management_lfn_settings_set(g_node.ws_if, &lfn);
    if (ret) tr_warn("ws_management_lfn_settings_set: %d", ret);
    ret = ws_management_lfn_parent_set(g_node.ws_if, true);
    if (ret) tr_warn("ws_management_lfn_parent_set: %d", ret);
    tr_info("FAN 1.1, LFN parenting on");
}

void node_test_mac_allow(const char *eui64_hex)
{
    uint8_t eui64[8];
    mlme_set_t req = { 0 };
    mlme_request_mac_filter_start_t start = { .lqi_m = 0, .lqi_add = 0, .dbm_m = 0, .dbm_add = 0 };
    mlme_request_mac_filter_add_long_t allow = { .lqi_m = 0x100, .lqi_add = 0, .dbm_m = 0x100, .dbm_add = 0 };

    for (int i = 0; i < 8; i++) {
        unsigned v;
        if (strlen(eui64_hex) != 16 || sscanf(eui64_hex + 2 * i, "%2x", &v) != 1) {
            tr_err("FW_TEST_MAC_ALLOW: '%s' is not 16 hex digits, no filter", eui64_hex);
            return;
        }
        eui64[i] = v;
    }
    /* Default: every source blocked (MAC_FILTER_BLOCKED); this EUI-64 passes
     * unchanged (MAC_FILTER_ALLOWED). Frames without a source address pass. */
    req.attr = macFilterStart;
    req.value_pointer = &start;
    req.value_size = sizeof(start);
    g_node.mac->mlme_req(g_node.mac, MLME_SET, &req);
    memcpy(allow.mac64, eui64, 8);
    req.attr = macFilterAddLong;
    req.value_pointer = &allow;
    req.value_size = sizeof(allow);
    g_node.mac->mlme_req(g_node.mac, MLME_SET, &req);
    tr_warn("TEST MODE: frames accepted only from %s (FW_TEST_MAC_ALLOW)", trace_array(eui64, 8));
}

void node_print_status(bool verbose)
{
    ws_stack_info_t info;
    char a[48];
    struct ns_port_heap_stats hs;
    int n = 0;
    uint8_t addr[16];

    ns_port_heap_stats(&hs);
    ns_port_console_printf("== %s %02x%02x%02x%02x%02x%02x%02x%02x up %lus ready=%d (first after %lus, %lu times) heap free %u/%u min %u\r\n",
                           app_role_name, g_node.eui64[0], g_node.eui64[1], g_node.eui64[2], g_node.eui64[3],
                           g_node.eui64[4], g_node.eui64[5], g_node.eui64[6], g_node.eui64[7],
                           (unsigned long)((now_ms() - g_node.boot_ms) / 1000), g_node.bootstrap_ready,
                           (unsigned long)(g_node.ready_ms / 1000), (unsigned long)g_node.ready_count,
                           (unsigned)hs.free_now, (unsigned)hs.total, (unsigned)hs.free_min_ever);
    if (ws_stack_info_get(g_node.ws_if, &info) == 0) {
        ip6tos(info.parent, a);
        ns_port_console_printf("   join_state=%u pan_id=0x%04x parent=%s rsl_in=%d rsl_out=%d routing_cost=%u\r\n",
                               info.join_state, info.pan_id, a, (int)info.rsl_in - 174,
                               (int)info.rsl_out - 174, info.routing_cost);
    }
    while (arm_net_address_list_get_next(g_node.ws_if, &n, addr) >= 0) {
        ip6tos(addr, a);
        ns_port_console_printf("   ws0 addr %s\r\n", a);
    }
    app_role_status(verbose);
    if (verbose) {
        struct rail_phy_stats rs;
        struct ns_port_stats ps;

        rail_phy_get_stats(&rs);
        ns_port_get_stats(&ps);
        ns_port_console_printf("   radio: ch=%u rx_ok=%lu rx_crc=%lu rx_ovf=%lu rx_ringfull=%lu | tx_req=%lu ok=%lu cca_busy=%lu cca_busy_rx=%lu fail=%lu missed=%lu late=%lu aborted=%lu\r\n",
                               rs.channel, (unsigned long)rs.rx_ok, (unsigned long)rs.rx_crc_err,
                               (unsigned long)rs.rx_fifo_overflow, (unsigned long)rs.rx_ring_full,
                               (unsigned long)rs.tx_requests, (unsigned long)rs.tx_ok, (unsigned long)rs.tx_cca_busy,
                               (unsigned long)rs.tx_cca_busy_rx, (unsigned long)rs.tx_fail,
                               (unsigned long)rs.tx_sched_missed, (unsigned long)rs.tx_late_start,
                               (unsigned long)rs.tx_aborted_by_mac);
        ns_port_console_printf("   radio: sched_refused=%lu tx_start_err=%lu (last status 0x%lx, state 0x%lx) tx_underflow/other=%lu tx_lost=%lu\r\n",
                               (unsigned long)rs.tx_sched_refused,
                               (unsigned long)rs.tx_start_err, (unsigned long)rs.tx_start_err_last,
                               (unsigned long)rs.tx_start_err_state, (unsigned long)rs.tx_underflow,
                               (unsigned long)rs.tx_lost);
        ns_port_console_printf("   radio: rx_aborted=%lu rx_hop_abort=%lu rx_hop_deferred=%lu rx_hop_defer_max=%luus ack_on_rx_ch=%lu\r\n",
                               (unsigned long)rs.rx_aborted, (unsigned long)rs.rx_hop_abort,
                               (unsigned long)rs.rx_hop_deferred, (unsigned long)rs.rx_hop_defer_max_us,
                               (unsigned long)rs.ack_on_rx_channel);
        ns_port_console_printf("   radio: ack_tx=%lu ack_lat_max=%luus prepare_late_max=%luus cca_thr=%ddBm txpwr=%d.%ddBm cal=%lu\r\n",
                               (unsigned long)rs.ack_tx, (unsigned long)rs.ack_latency_max_us,
                               (unsigned long)rs.prepare_lateness_max_us, rs.cca_threshold_dbm,
                               rs.tx_power_ddbm / 10, (rs.tx_power_ddbm < 0 ? -rs.tx_power_ddbm : rs.tx_power_ddbm) % 10,
                               (unsigned long)rs.cal_runs);
        ns_port_console_printf("   port: loops=%lu timer_cb=%lu fhss_cb=%lu timer_late_max=%luus cb_timers=%d trace_dropped=%lu stack_hwm=%luB\r\n",
                               (unsigned long)ps.loop_iterations, (unsigned long)ps.timer_callbacks,
                               (unsigned long)ps.fhss_callbacks, (unsigned long)ps.max_timer_lateness_us,
                               ns_port_callback_timers_used(), (unsigned long)ns_port_trace_dropped(),
                               (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    }
}

static void stack_task(void *arg)
{
    (void)arg;
    mac_api_t *mac;
    static mac_description_storage_size_t storage_sizes = {
        .device_description_table_size = APP_MAC_DEVICE_TABLE_SIZE,
#ifdef APP_LFN_PARENT
        /* 4 GTKs and 3 LGTKs (key indexes 5-7, the stack's LFN_STACK.md §8) */
        .key_description_table_size = 7,
#else
        .key_description_table_size = 4,
#endif
        .key_lookup_size = 1,
        .key_usage_size = 3,
    };

    platform_critical_init();
    ns_port_trace_init();
    mbed_trace_init();
    mbed_trace_print_function_set(ns_port_trace_write);
    mbed_trace_config_set(TRACE_ACTIVE_LEVEL_INFO);
    fault_report_previous();

#ifndef APP_ROLE_LFN
    /* Always-on FFN: the radio receives continuously, never go below EM1. */
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
#endif

    rail_phy_hw_init();
#ifdef APP_ROLE_LFN
    /* LFN: EM2 between its windows, RAIL time kept across sleep (HW8 H3) */
    if (rail_phy_sleep_init()) {
        tr_err("rail_phy_sleep_init failed: the LFN will not sleep in EM2");
        sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
    }
#endif
    ns_port_init();
    g_node.boot_ms = now_ms();
    {
        RAIL_Version_t v;
        RAIL_GetVersion(&v, false);
        tr_info("nanostack-efr32fg25-freertos %s starting, RAIL %u.%u.%u", app_role_name,
                v.major, v.minor, v.rev);
    }
    if (rail_phy_check_irq_priorities()) {
        tr_warn("some RAIL IRQs have a priority above configMAX_SYSCALL_INTERRUPT_PRIORITY: "
                "critical sections do not mask them");
    }

    eventOS_scheduler_init();
    rail_phy_get_eui64(g_node.eui64);
    g_node.rf_id = rail_phy_register(g_node.eui64);
    if (g_node.rf_id < 0) {
        tr_err("rail_phy_register: %d", g_node.rf_id);
        configASSERT(0);
    }
    mac = ns_sw_mac_create(g_node.rf_id, &storage_sizes);
    configASSERT(mac);
    g_node.mac = mac;
    if (net_init_core()) {
        tr_err("net_init_core");
        configASSERT(0);
    }
    /* arm_nwk_interface_lowpan_init() calls mac->mac_initialize() itself. */
    g_node.ws_if = arm_nwk_interface_lowpan_init(mac, (char *)"ws0");
    configASSERT(g_node.ws_if >= 0);
    app_role_create_interfaces();
    eventOS_event_handler_create(app_tasklet, ARM_LIB_TASKLET_INIT_EVENT);
    cli_init();
    ns_port_add_poll_hook(cli_poll);
    ns_port_add_poll_hook(ns_port_random_poll);
    ns_port_run();
}

/* sl_main wraps main(): this runs in the SDK start task once the kernel is up. */
int main(void)
{
    sl_main_second_stage_init();
    xTaskCreateStatic(stack_task, "ns_stack", sizeof(stack_task_stack) / sizeof(StackType_t), NULL,
                      configMAX_PRIORITIES - 2, stack_task_stack, &stack_task_tcb);
    return 0;  /* the start task terminates, the stack task runs */
}
