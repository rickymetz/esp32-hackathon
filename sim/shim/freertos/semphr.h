/* Host shim for freertos/semphr.h -- a single-threaded recursive-safe mutex.
 * The sim never runs the LVGL binding from more than one thread, so a mutex is
 * a simple ownership counter; Take/Give always succeed. */
#ifndef SIM_FREERTOS_SEMPHR_H
#define SIM_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct sim_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
void              vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t sem);

#endif /* SIM_FREERTOS_SEMPHR_H */
