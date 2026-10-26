/*
 * Trace/console output (GENERICITY_PLAN.md §3.1.8): a non-blocking byte ring
 * filled by the stack task (mbed_trace print function, CLI) and drained to
 * the VCOM iostream (EUSART, 115200 8N1) by a low-priority task, so a slow
 * UART never stalls the MAC. Whole lines that do not fit are dropped and
 * counted (the MAC has a few very chatty traces).
 *
 * Each trace line gets a "[ms] " prefix from the stack task's clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "sl_iostream.h"
#include "sl_iostream_handles.h"
#include "sl_iostream_uart.h"
#include "sl_iostream_init_eusart_instances.h"

#include "ns_port/critical.h"
#include "ns_port.h"

#ifndef NS_PORT_TRACE_RING_SIZE
#define NS_PORT_TRACE_RING_SIZE 8192
#endif
#define LOG_TASK_STACK_WORDS 256
#define LOG_TASK_PRIO        (tskIDLE_PRIORITY + 1)

static uint8_t ring[NS_PORT_TRACE_RING_SIZE];
static volatile uint32_t head, tail;  /* head: write index, tail: read index (free-running) */
static volatile uint32_t dropped;
static TaskHandle_t log_task;

static StaticTask_t log_task_tcb;
static StackType_t log_task_stack[LOG_TASK_STACK_WORDS];

static bool ring_put(const char *s, size_t len)
{
    bool ok;

    platform_enter_critical();
    ok = (NS_PORT_TRACE_RING_SIZE - (head - tail)) >= len;
    if (ok) {
        for (size_t i = 0; i < len; i++) {
            ring[(head + i) % NS_PORT_TRACE_RING_SIZE] = s[i];
        }
        head += len;
    } else {
        dropped++;
    }
    platform_exit_critical();
    if (ok && log_task) {
        xTaskNotifyGive(log_task);
    }
    return ok;
}

static void log_task_fn(void *arg)
{
    (void)arg;
    uint8_t chunk[64];

    for (;;) {
        uint32_t n = head - tail;
        if (!n) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = ring[(tail + i) % NS_PORT_TRACE_RING_SIZE];
        }
        /* Blocking write, in this low-priority task only. */
        sl_iostream_write(sl_iostream_vcom_handle, chunk, n);
        platform_enter_critical();
        tail += n;
        platform_exit_critical();
    }
}

void ns_port_trace_init(void)
{
    /* Non-blocking reads: the CLI polls with ns_port_console_getchar(). */
    sl_iostream_set_default(sl_iostream_vcom_handle);
    sl_iostream_uart_set_read_block(sl_iostream_uart_vcom_handle, false);
    log_task = xTaskCreateStatic(log_task_fn, "log_tx", LOG_TASK_STACK_WORDS, NULL,
                                 LOG_TASK_PRIO, log_task_stack, &log_task_tcb);
}

void ns_port_trace_write(const char *line)
{
    char pfx[16];
    size_t l = strlen(line);
    /* ms since boot (RTOS tick; the 32-bit µs RAIL clock wraps every 71 min) */
    int p = snprintf(pfx, sizeof(pfx), "[%lu] ", (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    char buf[256];

    /* One ring_put per line so that lines are dropped whole. */
    if ((size_t)p + l + 2 > sizeof(buf)) {
        l = sizeof(buf) - p - 2;
    }
    memcpy(buf, pfx, p);
    memcpy(buf + p, line, l);
    buf[p + l] = '\r';
    buf[p + l + 1] = '\n';
    ring_put(buf, p + l + 2);
}

uint32_t ns_port_trace_dropped(void)
{
    return dropped;
}

void ns_port_console_write(const char *s, size_t len)
{
    ring_put(s, len);
}

void ns_port_console_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ring_put(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

int ns_port_console_getchar(void)
{
    uint8_t c;
    size_t got = 0;

    /* Non-blocking read (set in ns_port_trace_init) from the EUSART RX buffer. */
    if (sl_iostream_read(sl_iostream_vcom_handle, &c, 1, &got) == SL_STATUS_OK && got == 1) {
        return c;
    }
    return -1;
}
