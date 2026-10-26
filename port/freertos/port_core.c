/*
 * Stack task loop, scheduler signal, µs callback timers and FHSS timers
 * (GENERICITY_PLAN.md §3.1.2, §3.1.3, §3.1.5, §3.1.6), Phase A model:
 * the RAIL multitimer ISR only marks a timer expired and notifies the stack
 * task; the callback runs in the stack task, from ns_port_run().
 *
 * Timebase: RAIL_GetTime() (µs, 32 bit), the same clock as the RAIL RX/TX
 * timestamps the radio driver hands to the MAC.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "rail.h"

#include "nanostack-event-loop/eventOS_scheduler.h"
#include "nanostack-event-loop/eventOS_callback_timer.h"
#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_config.h"
#include "ns_port/critical.h"
#include "ns_port.h"

#define NS_PORT_CALLBACK_TIMERS 16   /* plan: N >= 12, the stack registers ~6 */
#define NS_PORT_FHSS_TIMERS     4    /* FHSS uses 2-3 distinct callbacks */
#define NS_PORT_POLL_HOOKS      4
#define SLOT_US                 50   /* Nanostack callback timer slot */

static TaskHandle_t stack_task;

static void (*poll_hooks[NS_PORT_POLL_HOOKS])(void);
static unsigned poll_hook_count;

static struct ns_port_stats stats;

/* ---------------------------------------------------------------- notify */

void ns_port_notify(void)
{
    if (stack_task) {
        xTaskNotifyGive(stack_task);
    }
}

void ns_port_notify_from_isr(void)
{
    BaseType_t woken = pdFALSE;

    if (stack_task) {
        vTaskNotifyGiveFromISR(stack_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void eventOS_scheduler_signal(void)
{
    if (xPortIsInsideInterrupt()) {
        ns_port_notify_from_isr();
    } else {
        ns_port_notify();
    }
}

uint32_t ns_port_time_us(void)
{
    return RAIL_GetTime();
}

/* No real-time clock on the board. */
uint64_t ns_port_time_wall_s(void)
{
    return 0;
}

void ns_port_add_poll_hook(void (*hook)(void))
{
    configASSERT(poll_hook_count < NS_PORT_POLL_HOOKS);
    poll_hooks[poll_hook_count++] = hook;
}

/* -------------------------------------------------------- callback timers */

struct cb_timer {
    RAIL_MultiTimer_t hw;
    void (*fn)(int, uint16_t);
    uint32_t deadline;
    bool used;
    volatile bool expired;
};
static struct cb_timer cb_timers[NS_PORT_CALLBACK_TIMERS];
static volatile bool any_cb_expired;

static void cb_timer_isr(RAIL_MultiTimer_t *tmr, RAIL_Time_t expected, void *arg)
{
    (void)tmr;
    (void)expected;
    struct cb_timer *t = arg;

    t->expired = true;
    any_cb_expired = true;
    ns_port_notify_from_isr();
}

int eventOS_callback_timer_register(void (*timer_interrupt_handler)(int, uint16_t))
{
    for (int i = 0; i < NS_PORT_CALLBACK_TIMERS; i++) {
        if (!cb_timers[i].used) {
            memset(&cb_timers[i], 0, sizeof(cb_timers[i]));
            cb_timers[i].used = true;
            cb_timers[i].fn = timer_interrupt_handler;
            return i;
        }
    }
    return -1;
}

int eventOS_callback_timer_unregister(int ns_timer_id)
{
    if (ns_timer_id < 0 || ns_timer_id >= NS_PORT_CALLBACK_TIMERS) {
        return -1;
    }
    platform_enter_critical();
    RAIL_CancelMultiTimer(&cb_timers[ns_timer_id].hw);
    cb_timers[ns_timer_id].expired = false;
    cb_timers[ns_timer_id].used = false;
    platform_exit_critical();
    return 0;
}

int eventOS_callback_timer_start(int ns_timer_id, uint16_t slots)
{
    struct cb_timer *t;
    uint32_t delay_us = (uint32_t)slots * SLOT_US;

    if (ns_timer_id < 0 || ns_timer_id >= NS_PORT_CALLBACK_TIMERS || !cb_timers[ns_timer_id].used) {
        return -1;
    }
    t = &cb_timers[ns_timer_id];
    platform_enter_critical();
    /* Restart re-arms: drop a pending expiry of the previous arming. */
    RAIL_CancelMultiTimer(&t->hw);
    t->expired = false;
    t->deadline = RAIL_GetTime() + delay_us;
    if (delay_us == 0 ||
        RAIL_SetMultiTimer(&t->hw, delay_us, RAIL_TIME_DELAY, cb_timer_isr, t) != RAIL_STATUS_NO_ERROR) {
        /* Zero or rejected (already in the past): expire now. */
        t->expired = true;
        any_cb_expired = true;
    }
    platform_exit_critical();
    if (t->expired) {
        ns_port_notify();
    }
    return 0;
}

int eventOS_callback_timer_stop(int ns_timer_id)
{
    if (ns_timer_id < 0 || ns_timer_id >= NS_PORT_CALLBACK_TIMERS) {
        return -1;
    }
    platform_enter_critical();
    RAIL_CancelMultiTimer(&cb_timers[ns_timer_id].hw);
    cb_timers[ns_timer_id].expired = false;
    platform_exit_critical();
    return 0;
}

int ns_port_callback_timers_used(void)
{
    int n = 0;
    for (int i = 0; i < NS_PORT_CALLBACK_TIMERS; i++) {
        n += cb_timers[i].used;
    }
    return n;
}

static void note_lateness(uint32_t deadline)
{
    int32_t late = (int32_t)(RAIL_GetTime() - deadline);

    if (late > 0 && (uint32_t)late > stats.max_timer_lateness_us) {
        stats.max_timer_lateness_us = late;
    }
}

static void dispatch_cb_timers(void)
{
    if (!any_cb_expired) {
        return;
    }
    any_cb_expired = false;
    for (int i = 0; i < NS_PORT_CALLBACK_TIMERS; i++) {
        struct cb_timer *t = &cb_timers[i];
        bool fire;

        platform_enter_critical();
        fire = t->used && t->expired;
        t->expired = false;
        platform_exit_critical();
        if (fire) {
            note_lateness(t->deadline);
            stats.timer_callbacks++;
            t->fn(i, 0);
        }
    }
}

/* ------------------------------------------------------------ FHSS timers */

typedef void (*fhss_cb_t)(const fhss_api_t *api, uint16_t);

struct fhss_tmr {
    RAIL_MultiTimer_t hw;
    fhss_cb_t fn;
    const fhss_api_t *api;
    uint32_t deadline;
    volatile bool expired;
};
static struct fhss_tmr fhss_timers[NS_PORT_FHSS_TIMERS];
static volatile bool any_fhss_expired;

static void fhss_timer_isr(RAIL_MultiTimer_t *tmr, RAIL_Time_t expected, void *arg)
{
    (void)tmr;
    (void)expected;
    struct fhss_tmr *t = arg;

    t->expired = true;
    any_fhss_expired = true;
    ns_port_notify_from_isr();
}

static struct fhss_tmr *fhss_timer_find(fhss_cb_t fn, bool create)
{
    for (int i = 0; i < NS_PORT_FHSS_TIMERS; i++) {
        if (fhss_timers[i].fn == fn) {
            return &fhss_timers[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < NS_PORT_FHSS_TIMERS; i++) {
        if (!fhss_timers[i].fn) {
            fhss_timers[i].fn = fn;
            return &fhss_timers[i];
        }
    }
    return NULL;
}

static int port_fhss_timer_start(uint32_t us, fhss_cb_t fn, const fhss_api_t *api)
{
    struct fhss_tmr *t = fhss_timer_find(fn, true);

    if (!t) {
        configASSERT(0);
        return -1;
    }
    platform_enter_critical();
    RAIL_CancelMultiTimer(&t->hw);
    t->api = api;
    t->expired = false;
    t->deadline = RAIL_GetTime() + us;
    if (us == 0 || RAIL_SetMultiTimer(&t->hw, us, RAIL_TIME_DELAY, fhss_timer_isr, t) != RAIL_STATUS_NO_ERROR) {
        t->expired = true;
        any_fhss_expired = true;
    }
    platform_exit_critical();
    if (t->expired) {
        ns_port_notify();
    }
    return 0;
}

static int port_fhss_timer_stop(fhss_cb_t fn, const fhss_api_t *api)
{
    (void)api;
    struct fhss_tmr *t = fhss_timer_find(fn, false);

    if (!t) {
        return -1;
    }
    platform_enter_critical();
    RAIL_CancelMultiTimer(&t->hw);
    t->expired = false;
    platform_exit_critical();
    return 0;
}

static uint32_t port_fhss_get_remaining_slots(fhss_cb_t fn, const fhss_api_t *api)
{
    (void)api;
    struct fhss_tmr *t = fhss_timer_find(fn, false);

    if (!t) {
        return 0;
    }
    return RAIL_GetMultiTimer(&t->hw, RAIL_TIME_DELAY);
}

static uint32_t port_fhss_get_timestamp(const fhss_api_t *api)
{
    (void)api;
    return RAIL_GetTime();
}

const fhss_timer_t ns_port_fhss_timer = {
    .fhss_timer_start = port_fhss_timer_start,
    .fhss_timer_stop = port_fhss_timer_stop,
    .fhss_get_remaining_slots = port_fhss_get_remaining_slots,
    .fhss_get_timestamp = port_fhss_get_timestamp,
    .fhss_resolution_divider = 1,
};

static void dispatch_fhss_timers(void)
{
    if (!any_fhss_expired) {
        return;
    }
    any_fhss_expired = false;
    for (int i = 0; i < NS_PORT_FHSS_TIMERS; i++) {
        struct fhss_tmr *t = &fhss_timers[i];
        bool fire;

        platform_enter_critical();
        fire = t->fn && t->expired;
        t->expired = false;
        platform_exit_critical();
        if (fire) {
            note_lateness(t->deadline);
            stats.fhss_callbacks++;
            t->fn(t->api, 0);
        }
    }
}

/* -------------------------------------------------------------- run loop */

void ns_port_init(void)
{
    stack_task = xTaskGetCurrentTaskHandle();
    /* RAIL software timers on top of the RAIL protocol timer. RAIL_Init()
     * must have been called by the radio driver already. */
    RAIL_ConfigMultiTimer(true);
}

void ns_port_get_stats(struct ns_port_stats *st)
{
    *st = stats;
}

void ns_port_clear_stats(void)
{
    memset(&stats, 0, sizeof(stats));
}

void ns_port_run(void)
{
    for (;;) {
        stats.loop_iterations++;
        for (unsigned i = 0; i < poll_hook_count; i++) {
            poll_hooks[i]();
        }
        dispatch_fhss_timers();
        dispatch_cb_timers();
        eventOS_scheduler_run_until_idle();
        if (any_cb_expired || any_fhss_expired) {
            continue;
        }
        /* The 10 ms event-loop tick timer bounds this wait; the poll hooks
         * (radio bottom half, CLI) are woken by their ISRs. The timeout is
         * only a safety net for a missed notification. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}
