/* Host shim for FreeRTOS.h -- base types and tick helpers.
 * The sim runs single-threaded, so ticks are milliseconds and the scheduler
 * primitives are no-ops or trivial. */
#ifndef SIM_FREERTOS_H
#define SIM_FREERTOS_H

#include <stdint.h>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE   ((BaseType_t)1)
#define pdFALSE  ((BaseType_t)0)
#define pdPASS   pdTRUE
#define pdFAIL   pdFALSE

#define portMAX_DELAY       ((TickType_t)0xFFFFFFFF)
#define portTICK_PERIOD_MS  ((TickType_t)1)
#define configTICK_RATE_HZ  1000

#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))

/* Spinlock / critical sections: no-ops in the single-threaded sim. */
typedef struct { int unlocked; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(mux)       ((void)(mux))
#define portEXIT_CRITICAL(mux)        ((void)(mux))
#define portENTER_CRITICAL_ISR(mux)   ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)    ((void)(mux))

#endif /* SIM_FREERTOS_H */
