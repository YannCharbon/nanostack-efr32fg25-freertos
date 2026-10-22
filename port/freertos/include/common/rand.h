/*
 * Compatibility header: the stack includes "common/rand.h" (rand_* API,
 * Arm randLIB names). Implemented in port/freertos/random.c on the SE TRNG
 * (GENERICITY_PLAN.md §3.1.4, D1, D13).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_COMMON_RAND_H
#define NS_PORT_COMMON_RAND_H

#include <stdint.h>

void rand_add_seed(uint64_t seed);
uint8_t rand_get_8bit(void);
uint16_t rand_get_16bit(void);
uint32_t rand_get_32bit(void);
uint64_t rand_get_64bit(void);
void *rand_get_n_bytes_random(void *data_ptr, uint8_t count);
/* Uniform in [min..max], inclusive. */
uint16_t rand_get_random_in_range(uint16_t min, uint16_t max);
/* base * U(min_factor, max_factor) with 1.15 fixed-point factors, clamped. */
uint32_t rand_randomise_base(uint32_t base, uint16_t min_factor, uint16_t max_factor);

#endif
