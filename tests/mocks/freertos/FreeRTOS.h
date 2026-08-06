// Mock freertos/FreeRTOS.h — shadows the ESP-IDF SDK header (Phase I Task 8,
// item OO). Only the minimal subset Mesh/Enrollment's ring-buffer usage
// needs: base types + the ISR-yield macro. There is no real scheduler on
// host builds to yield to, so portYIELD_FROM_ISR() is a no-op here.
#pragma once

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef unsigned long TickType_t;

#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#endif

// Real portYIELD_FROM_ISR() optionally takes a BaseType_t
// (xHigherPriorityTaskWoken) argument on ESP-IDF. Accept either call shape
// used in this codebase and no-op both — nothing to yield to on host.
#define portYIELD_FROM_ISR(...) ((void)0)
