/*
 * Critical sections (GENERICITY_PLAN.md §3.1.1) on FreeRTOS / Cortex-M33.
 *
 * Task context: taskENTER_CRITICAL()/taskEXIT_CRITICAL(), which nest (the
 * kernel keeps uxCriticalNesting) and raise BASEPRI to
 * configMAX_SYSCALL_INTERRUPT_PRIORITY. The RAIL interrupts must therefore
 * have a priority numerically >= that value, so that they are masked too
 * (checked at start-up by rail_phy_check_irq_priorities()).
 *
 * ISR context: in Phase A no ISR calls into the stack, so this path is only
 * defensive. It nests with a small saved-mask stack.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "FreeRTOS.h"
#include "task.h"
#include "common/hal_interrupt.h"

#define ISR_NEST_MAX 8

static UBaseType_t isr_saved_mask[ISR_NEST_MAX];
static volatile unsigned isr_depth;

void platform_critical_init(void)
{
    isr_depth = 0;
}

void platform_enter_critical(void)
{
    if (xPortIsInsideInterrupt()) {
        UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
        configASSERT(isr_depth < ISR_NEST_MAX);
        isr_saved_mask[isr_depth++] = mask;
    } else {
        taskENTER_CRITICAL();
    }
}

void platform_exit_critical(void)
{
    if (xPortIsInsideInterrupt()) {
        configASSERT(isr_depth > 0);
        UBaseType_t mask = isr_saved_mask[--isr_depth];
        taskEXIT_CRITICAL_FROM_ISR(mask);
    } else {
        taskEXIT_CRITICAL();
    }
}
