/*
 * Minimal line-based CLI on the VCOM (GENERICITY_PLAN.md §5.6), polled from
 * the stack task (port poll hook), so commands call the stack directly.
 *
 *   status [-v]           join state, PAN, parent, addresses (+ radio/port stats)
 *   ping <ipv6> [n] [sz]  ICMPv6 echo
 *   udp <ipv6> <text>     UDP datagram to port 1234 (peer answers "ack:...")
 *   heap                  heap statistics
 *   tasks                 FreeRTOS task stack high-water marks
 *   trace <0-5> [grps]    runtime trace level (0 none .. 5 debug), optional group filter (e.g. fhss,app)
 *   radio clear           reset radio and port (timer lateness) statistics
 *   reset                 reboot
 *   nodes                 (BR) RPL routing table
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "em_device.h"

#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/ip6string.h"

#include "ns_port.h"
#include "rail_phy.h"
#include "node.h"

#define LINE_MAX_LEN 160
#define ARGS_MAX     8

static char line[LINE_MAX_LEN];
static unsigned line_len;

static void prompt(void)
{
    ns_port_console_printf("%s> ", app_role_name);
}

static bool parse_ipv6(const char *s, uint8_t out[16])
{
    return stoip6(s, strlen(s), out);
}

static void cmd_help(void)
{
    ns_port_console_printf(
        "commands: status [-v] | ping <ipv6> [count] [size] | udp <ipv6> <text> | heap | tasks |\r\n"
        "          trace <0-5> [groups] | radio clear | reset%s\r\n", strcmp(app_role_name, "BR") ? "" : " | nodes");
}

static void cmd_heap(void)
{
    struct ns_port_heap_stats hs;

    ns_port_heap_stats(&hs);
    ns_port_console_printf("heap: total %u free %u min-ever %u largest %u allocs %lu frees %lu failed %lu\r\n",
                           (unsigned)hs.total, (unsigned)hs.free_now, (unsigned)hs.free_min_ever,
                           (unsigned)hs.largest_free_block, (unsigned long)hs.alloc_count,
                           (unsigned long)hs.free_count, (unsigned long)hs.failed_count);
}

static void cmd_tasks(void)
{
#if (configUSE_TRACE_FACILITY == 1)
    TaskStatus_t st[12];
    UBaseType_t n = uxTaskGetSystemState(st, 12, NULL);

    for (UBaseType_t i = 0; i < n; i++) {
        ns_port_console_printf("  %-12s prio %2lu stack free %lu B\r\n", st[i].pcTaskName,
                               (unsigned long)st[i].uxCurrentPriority,
                               (unsigned long)st[i].usStackHighWaterMark * sizeof(StackType_t));
    }
#else
    ns_port_console_printf("  ns_stack stack free %lu B (configUSE_TRACE_FACILITY off)\r\n",
                           (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
#endif
}

static void cmd_trace(int level)
{
    static const uint8_t map[] = {
        TRACE_ACTIVE_LEVEL_NONE, TRACE_ACTIVE_LEVEL_CMD, TRACE_ACTIVE_LEVEL_ERROR,
        TRACE_ACTIVE_LEVEL_WARN, TRACE_ACTIVE_LEVEL_INFO, TRACE_ACTIVE_LEVEL_DEBUG,
    };
    if (level < 0 || level > 5) {
        ns_port_console_printf("trace level 0..5\r\n");
        return;
    }
    mbed_trace_config_set(map[level]);
}

static void execute(char *s)
{
    char *argv[ARGS_MAX];
    int argc = 0;
    uint8_t addr[16];

    for (char *tok = strtok(s, " \t"); tok && argc < ARGS_MAX; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (!argc) {
        return;
    }
    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
        cmd_help();
    } else if (!strcmp(argv[0], "status")) {
        node_print_status(argc > 1 && !strcmp(argv[1], "-v"));
    } else if (!strcmp(argv[0], "ping") && argc >= 2) {
        if (!parse_ipv6(argv[1], addr)) {
            ns_port_console_printf("bad address\r\n");
        } else if (ping_start(addr, argc > 2 ? atoi(argv[2]) : 4, argc > 3 ? atoi(argv[3]) : 16)) {
            ns_port_console_printf("ping busy\r\n");
        }
    } else if (!strcmp(argv[0], "udp") && argc >= 3) {
        /* Re-join the text words. */
        char text[LINE_MAX_LEN];
        text[0] = 0;
        for (int i = 2; i < argc; i++) {
            strncat(text, argv[i], sizeof(text) - strlen(text) - 2);
            if (i + 1 < argc) {
                strcat(text, " ");
            }
        }
        if (!parse_ipv6(argv[1], addr)) {
            ns_port_console_printf("bad address\r\n");
        } else {
            ns_port_console_printf("udp tx: %d\r\n", udp_test_send(addr, text));
        }
    } else if (!strcmp(argv[0], "heap")) {
        cmd_heap();
    } else if (!strcmp(argv[0], "tasks")) {
        cmd_tasks();
    } else if (!strcmp(argv[0], "trace") && (argc == 2 || argc == 3)) {
        /* trace <level> [group,group,...]: optional include filter (ns_trace
         * keeps the pointer, so the string lives in a static buffer). */
        static char groups[64];
        cmd_trace(atoi(argv[1]));
        if (argc == 3) {
            strncpy(groups, argv[2], sizeof(groups) - 1);
            mbed_trace_include_filters_set(groups);
        } else {
            mbed_trace_include_filters_set(NULL);
        }
    } else if (!strcmp(argv[0], "radio") && argc == 2 && !strcmp(argv[1], "clear")) {
        rail_phy_clear_stats();
        ns_port_clear_stats();
    } else if (!strcmp(argv[0], "reset")) {
        NVIC_SystemReset();
    } else if (!app_role_cli(argc, argv)) {
        ns_port_console_printf("unknown command, try 'help'\r\n");
    }
}

void cli_init(void)
{
    line_len = 0;
    cmd_help();
    prompt();
}

void cli_poll(void)
{
    int c;

    while ((c = ns_port_console_getchar()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (!line_len) {
                continue;
            }
            ns_port_console_write("\r\n", 2);
            line[line_len] = 0;
            execute(line);
            line_len = 0;
            prompt();
        } else if (c == 0x7f || c == 0x08) {
            if (line_len) {
                line_len--;
                ns_port_console_write("\b \b", 3);
            }
        } else if (c >= 0x20 && line_len < LINE_MAX_LEN - 1) {
            char ch = (char)c;
            line[line_len++] = ch;
            ns_port_console_write(&ch, 1);  /* echo */
        }
    }
}
