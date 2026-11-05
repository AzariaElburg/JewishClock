#pragma once
// PC stand-in (jcclock): FreeRTOS with REAL locks - in the full simulator the
// web server's handlers run on their own thread next to the main loop, as on
// the clock (core 0 / core 1), so the clock's locks must really lock.
#include <cstdint>

typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)

// Spinlock-style critical sections: one process-wide lock is plenty here.
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void sim_enter_critical();
void sim_exit_critical();
#define portENTER_CRITICAL(mux) sim_enter_critical()
#define portEXIT_CRITICAL(mux) sim_exit_critical()
