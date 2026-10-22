/*
 * Random numbers (GENERICITY_PLAN.md §3.1.4) from the Secure Engine TRNG.
 *
 * rand_get_8bit() is also the only entropy source of the TLS CTR_DRBG
 * (tls_sec_prot_lib.c registers it, D13), so this must be a real TRNG.
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

#include "common/rand.h"
#include "common/hal_interrupt.h"
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

void rand_add_seed(uint64_t seed)
{
    (void)seed; /* TRNG: nothing to seed */
}

void *rand_get_n_bytes_random(void *data_ptr, uint8_t count)
{
    ns_port_random_bytes(data_ptr, count);
    return data_ptr;
}

uint8_t rand_get_8bit(void)
{
    uint8_t v;
    ns_port_random_bytes(&v, sizeof(v));
    return v;
}

uint16_t rand_get_16bit(void)
{
    uint16_t v;
    ns_port_random_bytes(&v, sizeof(v));
    return v;
}

uint32_t rand_get_32bit(void)
{
    uint32_t v;
    ns_port_random_bytes(&v, sizeof(v));
    return v;
}

uint64_t rand_get_64bit(void)
{
    uint64_t v;
    ns_port_random_bytes(&v, sizeof(v));
    return v;
}

uint16_t rand_get_random_in_range(uint16_t min, uint16_t max)
{
    uint32_t span, limit, r;

    if (max <= min) {
        return min;
    }
    span = (uint32_t)max - min + 1;               /* 1..65536 */
    /* Rejection sampling on 16 bits to avoid modulo bias. */
    limit = 0x10000u - (0x10000u % span);
    do {
        r = rand_get_16bit();
    } while (r >= limit);
    return (uint16_t)(min + r % span);
}

uint32_t rand_randomise_base(uint32_t base, uint16_t min_factor, uint16_t max_factor)
{
    uint16_t factor = rand_get_random_in_range(min_factor, max_factor);
    uint64_t v = ((uint64_t)base * factor) >> 15; /* 1.15 fixed point */

    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}
