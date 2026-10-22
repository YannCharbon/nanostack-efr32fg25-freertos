/*
 * Host twin: the FreeRTOS subset the firmware uses, on pthreads.
 *
 * - Each task is a thread. "Interrupt" context = the fake RAIL threads
 *   (timer, medium), which run callbacks holding the kernel lock with a
 *   thread-local in_isr flag, so xPortIsInsideInterrupt() is true there.
 * - taskENTER_CRITICAL() takes the same (recursive) kernel lock: like BASEPRI
 *   masking on the target, it keeps "ISRs" out of a task critical section.
 * - Task notifications are counting semaphores.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "FreeRTOS.h"
#include "task.h"
#include "host_twin.h"

struct host_task {
    pthread_t th;
    TaskFunction_t fn;
    void *arg;
    char name[16];
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t notify;
};

static pthread_mutex_t kernel_lock;
static __thread int in_isr;
static __thread struct host_task *current;

void host_rtos_init(void)
{
    pthread_mutexattr_t a;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&kernel_lock, &a);
}

void host_isr_enter(void)
{
    pthread_mutex_lock(&kernel_lock);
    in_isr++;
}

void host_isr_exit(void)
{
    in_isr--;
    pthread_mutex_unlock(&kernel_lock);
}

void host_kernel_lock(void)
{
    pthread_mutex_lock(&kernel_lock);
}

void host_kernel_unlock(void)
{
    pthread_mutex_unlock(&kernel_lock);
}

void host_enter_critical(void)
{
    pthread_mutex_lock(&kernel_lock);
}

void host_exit_critical(void)
{
    pthread_mutex_unlock(&kernel_lock);
}

BaseType_t xPortIsInsideInterrupt(void)
{
    return in_isr > 0;
}

void host_assert_failed(const char *file, int line, const char *expr)
{
    fprintf(stderr, "ASSERT %s:%d: %s\n", file, line, expr);
    fflush(NULL);
    abort();
}

static void *task_trampoline(void *p)
{
    struct host_task *t = p;

    current = t;
    pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name, uint32_t depth, void *arg,
                               UBaseType_t prio, StackType_t *stack, StaticTask_t *tcb)
{
    struct host_task *t = calloc(1, sizeof(*t));

    (void)depth;
    (void)prio;
    (void)stack;
    (void)tcb;
    t->fn = fn;
    t->arg = arg;
    snprintf(t->name, sizeof(t->name), "%s", name);
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    pthread_create(&t->th, NULL, task_trampoline, t);
    return t;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return current;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    pthread_mutex_lock(&t->m);
    t->notify++;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken)
{
    xTaskNotifyGive(t);
    if (woken) {
        *woken = pdTRUE;
    }
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    struct host_task *t = current;
    uint32_t v;

    pthread_mutex_lock(&t->m);
    if (!t->notify && ticks) {
        if (ticks == portMAX_DELAY) {
            while (!t->notify) {
                pthread_cond_wait(&t->c, &t->m);
            }
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ticks / 1000;
            ts.tv_nsec += (long)(ticks % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            while (!t->notify) {
                if (pthread_cond_timedwait(&t->c, &t->m, &ts) == ETIMEDOUT) {
                    break;
                }
            }
        }
    }
    v = t->notify;
    if (v) {
        t->notify = clear ? 0 : v - 1;
    }
    pthread_mutex_unlock(&t->m);
    return v;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 0;
}

/* heap: the firmware's heap_wrap.c wraps malloc() onto these. */
void *__real_malloc(size_t size);
void __real_free(void *p);
static size_t heap_used, heap_peak, n_alloc, n_free;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

void *pvPortMalloc(size_t size)
{
    size_t *p;

    pthread_mutex_lock(&heap_lock);
    if (heap_used + size + 16 > configTOTAL_HEAP_SIZE) {
        pthread_mutex_unlock(&heap_lock);
        return NULL;   /* same budget as the target */
    }
    p = __real_malloc(size + 16);
    if (p) {
        p[0] = size + 16;
        heap_used += size + 16;
        if (heap_used > heap_peak) {
            heap_peak = heap_used;
        }
        n_alloc++;
    }
    pthread_mutex_unlock(&heap_lock);
    return p ? (uint8_t *)p + 16 : NULL;
}

void vPortFree(void *v)
{
    size_t *p;

    if (!v) {
        return;
    }
    p = (size_t *)((uint8_t *)v - 16);
    pthread_mutex_lock(&heap_lock);
    heap_used -= p[0];
    n_free++;
    pthread_mutex_unlock(&heap_lock);
    __real_free(p);
}

void vPortGetHeapStats(HeapStats_t *st)
{
    pthread_mutex_lock(&heap_lock);
    memset(st, 0, sizeof(*st));
    st->xAvailableHeapSpaceInBytes = configTOTAL_HEAP_SIZE - heap_used;
    st->xMinimumEverFreeBytesRemaining = configTOTAL_HEAP_SIZE - heap_peak;
    st->xSizeOfLargestFreeBlockInBytes = configTOTAL_HEAP_SIZE - heap_used;
    st->xNumberOfSuccessfulAllocations = n_alloc;
    st->xNumberOfSuccessfulFrees = n_free;
    pthread_mutex_unlock(&heap_lock);
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)((uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}
