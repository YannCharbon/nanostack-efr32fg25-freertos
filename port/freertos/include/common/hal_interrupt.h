/*
 * Compatibility header: the stack includes "common/hal_interrupt.h".
 * Critical sections (GENERICITY_PLAN.md §3.1.1), implemented in
 * port/freertos/critical.c. Names kept from Arm Nanostack.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_COMMON_HAL_INTERRUPT_H
#define NS_PORT_COMMON_HAL_INTERRUPT_H

void platform_critical_init(void);
void platform_enter_critical(void);
void platform_exit_critical(void);

#endif
