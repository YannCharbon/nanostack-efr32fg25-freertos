/*
 * UDP and ICMPv6 echo tests over the Nanostack socket API (stack task only).
 *
 * UDP: both images listen on APP_UDP_PORT. A received datagram is logged
 * ("udp rx ...") and answered with "ack:<text>" unless it already starts
 * with "ack:", which gives a two-way check from one CLI command.
 *
 * Ping: raw ICMPv6 socket (identifier 0xffff), echo requests one per second,
 * replies matched on id/seq, RTT from the RAIL µs clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdio.h>

#include "rail.h"
#include "mbed-client-libservice/ns_trace.h"
#include "mbed-client-libservice/ip6string.h"
#include "mbed-client-libservice/common_functions.h"
#include "nanostack-event-loop/eventOS_event_timer.h"
#include "nanostack/net_interface.h"
#include "nanostack/socket_api.h"
#include "nanostack/ns_address.h"

#include "ns_port.h"
#include "app_config.h"
#include "node.h"

#define TRACE_GROUP "test"

/* ------------------------------------------------------------------- UDP */

static int8_t udp_sid = -1;

static void udp_cb(void *arg)
{
    socket_callback_t *cb = arg;
    ns_address_t src;
    char buf[128];
    char a[48];
    int16_t n;

    if ((cb->event_type & SOCKET_EVENT_MASK) != SOCKET_DATA) {
        return;
    }
    n = socket_recvfrom(udp_sid, buf, sizeof(buf) - 1, 0, &src);
    if (n < 0) {
        return;
    }
    buf[n] = 0;
    ip6tos(src.address, a);
    ns_port_console_printf("udp rx from [%s]:%u (%d bytes): %s\r\n", a, src.identifier, n, buf);
    tr_info("udp rx from %s: %s", a, buf);
    if (strncmp(buf, "ack:", 4) != 0) {
        char reply[sizeof(buf) + 4];
        int l = snprintf(reply, sizeof(reply), "ack:%s", buf);
        src.identifier = APP_UDP_PORT;
        socket_sendto(udp_sid, &src, reply, l);
    }
}

void udp_test_init(void)
{
    udp_sid = socket_open(SOCKET_UDP, APP_UDP_PORT, udp_cb);
    if (udp_sid < 0) {
        tr_err("UDP socket open failed: %d", udp_sid);
    }
}

int udp_test_send(const uint8_t addr[16], const char *text)
{
    ns_address_t dst = { .type = ADDRESS_IPV6, .identifier = APP_UDP_PORT };

    if (udp_sid < 0) {
        return -1;
    }
    memcpy(dst.address, addr, 16);
    return socket_sendto(udp_sid, &dst, text, strlen(text));
}

/* ------------------------------------------------------------------ ping */

#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY   129
#define PING_ID             0x5753   /* "WS" */
#define PING_MAX_PAYLOAD    256

static int8_t icmp_sid = -1;
static struct {
    uint8_t addr[16];
    unsigned left, sent, received, size;
    uint16_t seq;
    uint32_t sent_at;
    uint32_t rtt_min, rtt_max, rtt_sum;
    bool active;
} ping;

static void icmp_cb(void *arg)
{
    socket_callback_t *cb = arg;
    uint8_t buf[8 + PING_MAX_PAYLOAD];
    ns_address_t src;
    int16_t n;
    char a[48];

    if ((cb->event_type & SOCKET_EVENT_MASK) != SOCKET_DATA) {
        return;
    }
    n = socket_recvfrom(icmp_sid, buf, sizeof(buf), 0, &src);
    if (n < 8 || buf[0] != ICMPV6_ECHO_REPLY) {
        return;
    }
    if (common_read_16_bit(buf + 4) != PING_ID || common_read_16_bit(buf + 6) != ping.seq || !ping.active) {
        return;
    }
    uint32_t rtt = (RAIL_GetTime() - ping.sent_at) / 1000u;
    ping.received++;
    ping.rtt_sum += rtt;
    if (rtt < ping.rtt_min) ping.rtt_min = rtt;
    if (rtt > ping.rtt_max) ping.rtt_max = rtt;
    ip6tos(src.address, a);
    ns_port_console_printf("%d bytes from %s: icmp_seq=%u time=%lu ms\r\n", n - 8, a, ping.seq,
                           (unsigned long)rtt);
}

void ping_init(void)
{
    icmp_sid = socket_open(SOCKET_ICMP, 0xffff, icmp_cb);
    if (icmp_sid < 0) {
        tr_err("ICMP socket open failed: %d", icmp_sid);
    }
}

static void ping_send_one(void)
{
    uint8_t buf[8 + PING_MAX_PAYLOAD];
    ns_address_t dst = { .type = ADDRESS_IPV6, .identifier = 0 };

    memcpy(dst.address, ping.addr, 16);
    ping.seq++;
    buf[0] = ICMPV6_ECHO_REQUEST;
    buf[1] = 0;
    buf[2] = buf[3] = 0;   /* checksum computed by the stack */
    common_write_16_bit(PING_ID, buf + 4);
    common_write_16_bit(ping.seq, buf + 6);
    for (unsigned i = 0; i < ping.size; i++) {
        buf[8 + i] = (uint8_t)i;
    }
    ping.sent_at = RAIL_GetTime();
    ping.sent++;
    if (socket_sendto(icmp_sid, &dst, buf, 4 + 4 + ping.size) < 0) {
        ns_port_console_printf("ping: send failed (no route?)\r\n");
    }
}

static void ping_summary(void)
{
    char a[48];

    ip6tos(ping.addr, a);
    ns_port_console_printf("--- %s ping statistics: %u transmitted, %u received, %u%% loss",
                           a, ping.sent, ping.received,
                           ping.sent ? (ping.sent - ping.received) * 100 / ping.sent : 0);
    if (ping.received) {
        ns_port_console_printf(", rtt min/avg/max = %lu/%lu/%lu ms", (unsigned long)ping.rtt_min,
                               (unsigned long)(ping.rtt_sum / ping.received), (unsigned long)ping.rtt_max);
    }
    ns_port_console_printf("\r\n");
}

int ping_start(const uint8_t addr[16], unsigned count, unsigned size)
{
    if (icmp_sid < 0 || ping.active) {
        return -1;
    }
    memset(&ping, 0, sizeof(ping));
    memcpy(ping.addr, addr, 16);
    ping.left = count ? count : 1;
    ping.size = size > PING_MAX_PAYLOAD ? PING_MAX_PAYLOAD : size;
    ping.rtt_min = UINT32_MAX;
    ping.active = true;
    ping_on_timer();
    return 0;
}

/* One step per second: send the next request, or print the summary 1 s
 * (+ a 3 s grace for multi-hop) after the last one. */
void ping_on_timer(void)
{
    if (!ping.active) {
        return;
    }
    if (ping.left) {
        ping.left--;
        ping_send_one();
        eventOS_event_timer_request(0, APP_EVENT_PING, g_node.tasklet, ping.left ? 1000 : 4000);
        return;
    }
    ping.active = false;
    ping_summary();
}
