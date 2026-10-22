/*
 * malloc/free/calloc/realloc of the whole image are routed to the FreeRTOS
 * heap_4 (GENERICITY_PLAN.md §3.1.7). Linked with
 *   -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
 * so the stack, mbedTLS and our code share one accounted, thread-safe heap
 * (configTOTAL_HEAP_SIZE). The stack has no allocation cap (D20), so the CLI
 * `heap` command reports the low-water mark.
 *
 * heap_4 has no realloc and no usable-size query, so each block carries an
 * 8-byte header with its requested size (keeps 8-byte alignment).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "ns_port.h"

#define HDR 8u

static volatile uint32_t n_alloc, n_free, n_fail;

void *__wrap_malloc(size_t size)
{
    uint8_t *p;

    if (size > SIZE_MAX - HDR) {
        n_fail++;
        return NULL;
    }
    p = pvPortMalloc(size + HDR);
    if (!p) {
        n_fail++;
        return NULL;
    }
    n_alloc++;
    *(size_t *)p = size;
    return p + HDR;
}

void __wrap_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    n_free++;
    vPortFree((uint8_t *)ptr - HDR);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    void *p;

    if (size && nmemb > SIZE_MAX / size) {
        n_fail++;
        return NULL;
    }
    p = __wrap_malloc(nmemb * size);
    if (p) {
        memset(p, 0, nmemb * size);
    }
    return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    void *n;
    size_t old;

    if (!ptr) {
        return __wrap_malloc(size);
    }
    if (!size) {
        __wrap_free(ptr);
        return NULL;
    }
    old = *(size_t *)((uint8_t *)ptr - HDR);
    n = __wrap_malloc(size);
    if (n) {
        memcpy(n, ptr, old < size ? old : size);
        __wrap_free(ptr);
    }
    return n;
}

#ifdef _NEWLIB_VERSION
/* newlib's reentrant entry points, in case some libc code uses them. */
void *__wrap__malloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return __wrap_malloc(size);
}

void __wrap__free_r(struct _reent *r, void *ptr)
{
    (void)r;
    __wrap_free(ptr);
}

void *__wrap__calloc_r(struct _reent *r, size_t nmemb, size_t size)
{
    (void)r;
    return __wrap_calloc(nmemb, size);
}

void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size)
{
    (void)r;
    return __wrap_realloc(ptr, size);
}
#endif /* _NEWLIB_VERSION */

void ns_port_heap_stats(struct ns_port_heap_stats *st)
{
    HeapStats_t hs;

    vPortGetHeapStats(&hs);
    st->total = configTOTAL_HEAP_SIZE;
    st->free_now = hs.xAvailableHeapSpaceInBytes;
    st->free_min_ever = hs.xMinimumEverFreeBytesRemaining;
    st->largest_free_block = hs.xSizeOfLargestFreeBlockInBytes;
    st->alloc_count = n_alloc;
    st->free_count = n_free;
    st->failed_count = n_fail;
}

void vApplicationMallocFailedHook(void)
{
    /* Counted in n_fail by the wrappers; the stack handles NULL itself.
     * Kernel-object allocations failing would be a configuration error. */
}
