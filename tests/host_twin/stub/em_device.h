/* Host twin: just enough of the EFR32FG25 device header. */
#ifndef HOST_EM_DEVICE_H
#define HOST_EM_DEVICE_H
#include <stdint.h>
#define _SILICON_LABS_32B_SERIES_2 1
#define _SILICON_LABS_32B_SERIES 2
#define _SILICON_LABS_32B_SERIES_2_CONFIG 5
#define _SILICON_LABS_EFR32_RADIO_SUBGHZ 1
#define _SILICON_LABS_EFR32_RADIO_TYPE 2
#define __NVIC_PRIO_BITS 4
typedef enum {
    AGC_IRQn = 37, BUFC_IRQn, FRC_PRI_IRQn, FRC_IRQn, MODEM_IRQn, PROTIMER_IRQn,
    RAC_RSM_IRQn, RAC_SEQ_IRQn, HOSTMAILBOX_IRQn, SYNTH_IRQn,
} IRQn_Type;
#define __DMB() __sync_synchronize()
#define __BKPT(x) __builtin_trap()
uint32_t NVIC_GetPriority(IRQn_Type irq);
void NVIC_SystemReset(void) __attribute__((noreturn));
#endif
