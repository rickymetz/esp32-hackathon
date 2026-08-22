/* Host shim for freertos/task.h -- vTaskDelay maps to a real host sleep. */
#ifndef SIM_FREERTOS_TASK_H
#define SIM_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;

void vTaskDelay(TickType_t ticks);

/* Single-threaded sim: a stable non-NULL identity for the one "task". */
TaskHandle_t xTaskGetCurrentTaskHandle(void);

#endif /* SIM_FREERTOS_TASK_H */
