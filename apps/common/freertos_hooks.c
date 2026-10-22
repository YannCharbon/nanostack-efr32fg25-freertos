/*
 * FreeRTOS application hooks required by the SDK FreeRTOSConfig.h
 * (configCHECK_FOR_STACK_OVERFLOW = 2).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "FreeRTOS.h"
#include "task.h"
#include "em_device.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    /* No safe way to log from here: stop under the debugger, else reboot. */
    __BKPT(0);
    NVIC_SystemReset();
}
