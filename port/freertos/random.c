/*
 * Random numbers (GENERICITY_PLAN.md §3.1.4) from the Secure Engine TRNG.
 *
 * The stack's rand_* helpers (its ns_rand.c) are built on
 * ns_port_random_bytes(), and rand_get_8bit() is an entropy source of the TLS
 * CTR_DRBG (tls_sec_prot_lib.c registers it, D13), so this must be a real TRNG.
 * Bytes are served from a 64-byte pool refilled with sl_se_get_random() in
 * task context. Not ISR-safe (the SE mailbox may block): the stack only calls
 * it from the stack task in the Phase A model.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "sl_se_manager.h"
#include "sl_se_manager_entropy.h"

#include "ns_port/random.h"
#include "ns_port.h"

#define POOL_SIZE 64

static uint8_t pool[POOL_SIZE];
static unsigned pool_left;
static uint32_t trng_errors;


static void pool_refill(void)
{
    sl_se_command_context_t ctx;
    sl_status_t st;

    st = sl_se_init_command_context(&ctx);
    if (st == SL_STATUS_OK) {
        st = sl_se_get_random(&ctx, pool, sizeof(pool));
    }
    if (st != SL_STATUS_OK) {
        /* Should never happen. Do not silently continue with a predictable
         * pool: this feeds TLS. */
        trng_errors++;
        configASSERT(0);
    }
    pool_left = sizeof(pool);
}

/* Poll hook (stack task, outside any critical section): keep the pool at
 * least half full so that callers inside critical sections (e.g. the FHSS
 * broadcast handler) almost never trigger a blocking SE refill. */
void ns_port_random_poll(void)
{
    if (pool_left < POOL_SIZE / 2) {
        pool_refill();
    }
}

void ns_port_random_bytes(void *buf, size_t n)
{
    uint8_t *out = buf;

    configASSERT(!xPortIsInsideInterrupt());
    while (n) {
        if (!pool_left) {
            pool_refill();
        }
        size_t chunk = n < pool_left ? n : pool_left;
        memcpy(out, &pool[POOL_SIZE - pool_left], chunk);
        /* Do not leave served bytes in RAM. */
        memset(&pool[POOL_SIZE - pool_left], 0, chunk);
        pool_left -= chunk;
        out += chunk;
        n -= chunk;
    }
}

/* MBEDTLS_ENTROPY_HARDWARE_ALT: mbedTLS registers this as a STRONG entropy
 * source (config/mbedtls_config_target.h). */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    ns_port_random_bytes(output, len);
    *olen = len;
    return 0;
}
