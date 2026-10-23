/*
 * Host twin: the few Simplicity SDK services the firmware touches
 * (sl_main, power manager, VCOM iostream, SE TRNG, DEVINFO EUI-64, NVIC,
 * PA config), plus the process entry point.
 *
 * Environment:
 *   HOST_TWIN_NODE   index of this node (0..N-1), also the EUI-64 low byte
 *   HOST_TWIN_NODES  N, number of node processes on the medium
 *   HOST_TWIN_DIR    directory holding the medium sockets n<i>.sock
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "em_device.h"
#include "em_system.h"
#include "sl_main_init.h"
#include "sl_power_manager.h"
#include "sl_iostream_uart.h"
#include "sl_iostream_init_eusart_instances.h"
#include "sl_se_manager_entropy.h"
#include "pa_conversions_efr32.h"
#include "host_twin.h"

int host_node_index;
int host_node_count = 2;
const char *host_medium_dir = ".";

void sl_main_second_stage_init(void)
{
}

void sl_power_manager_add_em_requirement(sl_power_manager_em_t em)
{
    (void)em;
}

static sl_iostream_uart_t vcom = { .stream = { .fd = 1 } };
sl_iostream_t *sl_iostream_vcom_handle = &vcom.stream;
sl_iostream_uart_t *sl_iostream_uart_vcom_handle = &vcom;

sl_status_t sl_iostream_set_default(sl_iostream_t *s)
{
    (void)s;
    return SL_STATUS_OK;
}

sl_status_t sl_iostream_write(sl_iostream_t *s, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t n = write(s->fd, p, len);
        if (n <= 0) {
            return SL_STATUS_FAIL;
        }
        p += n;
        len -= n;
    }
    return SL_STATUS_OK;
}

sl_status_t sl_iostream_read(sl_iostream_t *s, void *buf, size_t len, size_t *got)
{
    (void)s;
    ssize_t n = read(0, buf, len);

    *got = n > 0 ? (size_t)n : 0;
    return n > 0 ? SL_STATUS_OK : SL_STATUS_FAIL;
}

void sl_iostream_uart_set_read_block(sl_iostream_uart_t *u, bool on)
{
    (void)u;
    int fl = fcntl(0, F_GETFL);
    fcntl(0, F_SETFL, on ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
}

sl_status_t sl_se_init_command_context(sl_se_command_context_t *c)
{
    (void)c;
    return SL_STATUS_OK;
}

sl_status_t sl_se_get_random(sl_se_command_context_t *c, void *buf, uint32_t len)
{
    (void)c;
    return getrandom(buf, len, 0) == (ssize_t)len ? SL_STATUS_OK : SL_STATUS_FAIL;
}

uint64_t SYSTEM_GetUnique(void)
{
    /* 02:00:5e:ef:fe:00:00:<node+1> (locally administered) */
    return 0x02005eeffe000000ull | (uint64_t)(host_node_index + 1);
}

uint32_t NVIC_GetPriority(IRQn_Type irq)
{
    (void)irq;
    return 5;
}

void NVIC_SystemReset(void)
{
    fprintf(stderr, "NVIC_SystemReset\n");
    exit(0);
}

RAIL_TxPowerConfig_t *sl_rail_util_pa_get_tx_power_config_subghz(void)
{
    static RAIL_TxPowerConfig_t cfg;
    return &cfg;
}

int __real_main(void);

int __wrap_main(void)
{
    const char *e;

    if ((e = getenv("HOST_TWIN_NODE"))) host_node_index = atoi(e);
    if ((e = getenv("HOST_TWIN_NODES"))) host_node_count = atoi(e);
    if ((e = getenv("HOST_TWIN_DIR"))) host_medium_dir = e;
    signal(SIGPIPE, SIG_IGN);
    host_rtos_init();
    __real_main();          /* apps/common/node.c main(): creates the stack task */
    for (;;) {
        pause();
    }
}

/* nanostack/mac/platform/os_whiteboard.h: a real function when __linux__ is
 * defined (the host build), a no-op macro on the target. Keep it a no-op. */
void whiteboard_os_modify(const uint8_t address[static 16], int mode)
{
    (void)address;
    (void)mode;
}

void fault_report_previous(void)
{
}
