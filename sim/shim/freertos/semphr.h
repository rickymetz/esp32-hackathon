/* Host shim for freertos/semphr.h.
 *
 * Two kinds, because the binding uses them for two different jobs:
 *
 *  - MUTEX (xSemaphoreCreateMutex): the sim never runs the LVGL binding from
 *    more than one thread, so this is a plain ownership counter and
 *    Take/Give always succeed immediately.
 *
 *  - BINARY (xSemaphoreCreateBinary): the event signal the LVGL task gives
 *    and the script task waits on. In the sim there IS no other task -- the
 *    same thread does both -- so a wait can never be satisfied by someone
 *    else and Take must sleep out its timeout, exactly as the vTaskDelay it
 *    replaced did. Anything else would busy-spin the drain loop. */
#ifndef SIM_FREERTOS_SEMPHR_H
#define SIM_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct sim_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void              vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t sem);

#endif /* SIM_FREERTOS_SEMPHR_H */
