/* Host twin: minimal FreeRTOS API on pthreads (see ../fake_rtos.c). */
#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
#include <assert.h>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef struct host_task *TaskHandle_t;
typedef struct { uint8_t opaque[64]; } StaticTask_t;
typedef struct {
    size_t xAvailableHeapSpaceInBytes;
    size_t xSizeOfLargestFreeBlockInBytes;
    size_t xSizeOfSmallestFreeBlockInBytes;
    size_t xNumberOfFreeBlocks;
    size_t xMinimumEverFreeBytesRemaining;
    size_t xNumberOfSuccessfulAllocations;
    size_t xNumberOfSuccessfulFrees;
} HeapStats_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1u
#define tskIDLE_PRIORITY 0
#define configMAX_PRIORITIES 56
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 48
#define configTOTAL_HEAP_SIZE (200 * 1024)
#define configUSE_TRACE_FACILITY 0
#define configASSERT(x) do { if (!(x)) host_assert_failed(__FILE__, __LINE__, #x); } while (0)

void host_assert_failed(const char *file, int line, const char *expr) __attribute__((noreturn));

void *pvPortMalloc(size_t size);
void vPortFree(void *p);
void vPortGetHeapStats(HeapStats_t *st);
BaseType_t xPortIsInsideInterrupt(void);
#define portYIELD_FROM_ISR(x) ((void)(x))

void host_enter_critical(void);
void host_exit_critical(void);
#define taskENTER_CRITICAL() host_enter_critical()
#define taskEXIT_CRITICAL() host_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR() ((UBaseType_t)0)
#define taskEXIT_CRITICAL_FROM_ISR(x) ((void)(x))

#endif
