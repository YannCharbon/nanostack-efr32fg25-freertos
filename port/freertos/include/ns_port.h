/*
 * Nanostack porting layer for FreeRTOS on EFR32 (Series 2).
 *
 * Implements the porting API of nanostack-generic-standalone
 * docs/GENERICITY_PLAN.md §3.1 ("Phase A" threading model):
 *  - the stack is single-threaded: every stack API is called from the one
 *    stack task, the one that runs ns_port_run();
 *  - ISRs (RAIL, timers) only capture data, set flags and notify that task;
 *  - ISR-safe functions: eventOS_scheduler_signal(), platform_enter/exit_critical(),
 *    ns_port_time_us(), ns_port_notify_from_isr().
 *
 * The functions the stack itself calls keep their Nanostack names and live in
 * the compatibility headers next to this one (common/hal_interrupt.h,
 * common/rand.h) and in nanostack-event-loop's eventOS_*.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_H
#define NS_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fhss_timer;

/* Once, from the stack task, after the radio timebase is up (RAIL_Init) and
 * before eventOS_scheduler_init(). Records the calling task as stack task. */
void ns_port_init(void);

/* Stack task main loop. Each iteration: run poll hooks (driver bottom half,
 * CLI), dispatch expired callback timers and FHSS timers, run the event loop
 * until idle, then block until notified. Never returns. */
void ns_port_run(void) __attribute__((noreturn));

/* Extra work done in each loop iteration, in stack context (max 4 hooks). */
void ns_port_add_poll_hook(void (*hook)(void));

/* Wake the stack task (from a task or an ISR). */
void ns_port_notify(void);
void ns_port_notify_from_isr(void);

/* Monotonic µs time, wraps at 2^32 (~71 min). Same timebase as the radio
 * timestamps (RAIL_GetTime()). ISR-safe. */
uint32_t ns_port_time_us(void);

/* FHSS platform timer for ws_management_node_init() (fhss_timer_t). */
extern const struct fhss_timer ns_port_fhss_timer;

/* Strong random bytes from the SE TRNG (seeds the TLS DRBG, D13). Task
 * context; may block shortly on the SE mailbox. */
void ns_port_random_bytes(void *buf, size_t n);

/* Heap (FreeRTOS heap_4; malloc/free/calloc/realloc are wrapped onto it). */
struct ns_port_heap_stats {
    size_t total;
    size_t free_now;
    size_t free_min_ever;
    size_t largest_free_block;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t failed_count;
};
void ns_port_heap_stats(struct ns_port_heap_stats *st);

/* Trace output (mbed_trace print function): non-blocking ring buffer drained
 * to the VCOM by a low-priority task. Lines that do not fit are dropped and
 * counted. */
void ns_port_trace_init(void);
void ns_port_trace_write(const char *line);
uint32_t ns_port_trace_dropped(void);
/* Raw console output for the CLI (same ring, same task). */
void ns_port_console_write(const char *s, size_t len);
void ns_port_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Console input: non-blocking read of one char, -1 if none. */
int ns_port_console_getchar(void);

/* Port statistics, for the CLI. */
struct ns_port_stats {
    uint32_t loop_iterations;
    uint32_t timer_callbacks;
    uint32_t fhss_callbacks;
    uint32_t max_timer_lateness_us;   /* deadline -> callback in stack task */
};
void ns_port_get_stats(struct ns_port_stats *st);

/* Callback timer slots in use / available (diagnostics). */
int ns_port_callback_timers_used(void);

#ifdef __cplusplus
}
#endif

#endif /* NS_PORT_H */
