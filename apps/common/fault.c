/*
 * Crash capture: fault handlers and the stack-overflow hook store the
 * exception frame and fault status registers in a .noinit record, then reset.
 * The next boot prints the record (fault_report_previous()), so a crash on
 * the bench shows up in the VCOM log instead of a silent hang.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "em_device.h"

#include "ns_port.h"

#define CRASH_MAGIC 0xC0FFEE42u

struct crash_record {
    uint32_t magic;
    uint32_t kind;          /* 1 = fault, 2 = stack overflow, 3 = assert */
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
    uint32_t exc_return, sp, cfsr, hfsr, bfar, mmfar, icsr;
    char task[16];
    uint32_t line;
    char file[48];
    uint32_t stack[256];     /* words above the exception frame (call chain hints) */
};

static struct crash_record crash __attribute__((section(".noinit")));

static void record_task(void)
{
    const char *n = NULL;

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        n = pcTaskGetName(NULL);
    }
    memset(crash.task, 0, sizeof(crash.task));
    if (n) {
        strncpy(crash.task, n, sizeof(crash.task) - 1);
    }
}

__attribute__((used)) void fault_capture(uint32_t *frame, uint32_t exc_return)
{
    crash.magic = CRASH_MAGIC;
    crash.kind = 1;
    crash.r0 = frame[0];
    crash.r1 = frame[1];
    crash.r2 = frame[2];
    crash.r3 = frame[3];
    crash.r12 = frame[4];
    crash.lr = frame[5];
    crash.pc = frame[6];
    crash.psr = frame[7];
    crash.exc_return = exc_return;
    crash.sp = (uint32_t)frame;
    crash.cfsr = SCB->CFSR;
    crash.hfsr = SCB->HFSR;
    crash.bfar = SCB->BFAR;
    crash.mmfar = SCB->MMFAR;
    crash.icsr = SCB->ICSR;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t a = (uint32_t)(frame + 8 + i);
        crash.stack[i] = (a >= 0x20000000u && a < 0x20080000u) ? frame[8 + i] : 0;
    }
    record_task();
    __DSB();
    NVIC_SystemReset();
}

/* Pick MSP or PSP from EXC_RETURN bit 2 and hand the frame to C. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b fault_capture\n");
}

__attribute__((naked)) void BusFault_Handler(void)
{
    __asm volatile("b HardFault_Handler\n");
}

__attribute__((naked)) void UsageFault_Handler(void)
{
    __asm volatile("b HardFault_Handler\n");
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile("b HardFault_Handler\n");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    memset(&crash, 0, sizeof(crash));
    crash.magic = CRASH_MAGIC;
    crash.kind = 2;
    strncpy(crash.task, name ? name : "?", sizeof(crash.task) - 1);
    __DSB();
    NVIC_SystemReset();
}

void fault_record_assert(const char *file, int line)
{
    const char *base = strrchr(file, '/');

    memset(&crash, 0, sizeof(crash));
    crash.magic = CRASH_MAGIC;
    crash.kind = 3;
    crash.line = line;
    strncpy(crash.file, base ? base + 1 : file, sizeof(crash.file) - 1);
    record_task();
    __DSB();
    NVIC_SystemReset();
}

void fault_report_previous(void)
{
    if (crash.magic != CRASH_MAGIC) {
        return;
    }
    crash.task[sizeof(crash.task) - 1] = 0;
    crash.file[sizeof(crash.file) - 1] = 0;
    if (crash.kind == 2) {
        ns_port_console_printf("!! previous boot: STACK OVERFLOW in task %s\r\n", crash.task);
    } else if (crash.kind == 3) {
        ns_port_console_printf("!! previous boot: ASSERT %s:%lu (task %s)\r\n", crash.file,
                               (unsigned long)crash.line, crash.task);
    } else {
        ns_port_console_printf("!! previous boot: FAULT in task %s: pc=%08lx lr=%08lx psr=%08lx sp=%08lx exc_ret=%08lx\r\n"
                               "!!   cfsr=%08lx hfsr=%08lx bfar=%08lx mmfar=%08lx icsr=%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\r\n",
                               crash.task, (unsigned long)crash.pc, (unsigned long)crash.lr,
                               (unsigned long)crash.psr, (unsigned long)crash.sp, (unsigned long)crash.exc_return,
                               (unsigned long)crash.cfsr, (unsigned long)crash.hfsr, (unsigned long)crash.bfar,
                               (unsigned long)crash.mmfar, (unsigned long)crash.icsr, (unsigned long)crash.r0,
                               (unsigned long)crash.r1, (unsigned long)crash.r2, (unsigned long)crash.r3,
                               (unsigned long)crash.r12);
        /* Code addresses found on the stack (decode with addr2line). */
        ns_port_console_printf("!!   stack code addrs:");
        for (unsigned i = 0; i < 256; i++) {
            uint32_t v = crash.stack[i];
            if (v >= 0x08000000u && v < 0x081e0000u && (v & 1)) {
                ns_port_console_printf(" %08lx", (unsigned long)(v & ~1u));
            }
        }
        ns_port_console_printf("\r\n");
    }
    crash.magic = 0;
}
