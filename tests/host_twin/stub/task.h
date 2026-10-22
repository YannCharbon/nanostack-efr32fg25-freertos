#ifndef HOST_TASK_H
#define HOST_TASK_H
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name, uint32_t depth, void *arg,
                               UBaseType_t prio, StackType_t *stack, StaticTask_t *tcb);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
#endif
