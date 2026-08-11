// Mock freertos/task.h — shadows the ESP-IDF SDK header (Phase I Task 9,
// item EE). Backs Mesh::drainNotifyHandle_ (TaskHandle_t) and the
// vTaskNotifyGiveFromISR() call in Mesh::onDataRecvCallback. There is no real
// FreeRTOS scheduler or task on host builds — Mesh::drainNotifyHandle_ is
// always nullptr in host/SimNode tests (main.cpp, which creates the real
// task and calls setDrainNotifyHandle(), is never compiled into host tests —
// see tests/CMakeLists.txt), so onDataRecvCallback's null-check always skips
// the notify path here. This mock only needs to exist so Mesh.h/Mesh.cpp
// compile; it is never exercised by a real notify on host.
#pragma once
#include "freertos/FreeRTOS.h"

typedef void* TaskHandle_t;

inline void vTaskNotifyGiveFromISR(TaskHandle_t /*xTaskToNotify*/,
                                   BaseType_t* pxHigherPriorityTaskWoken) {
  if (pxHigherPriorityTaskWoken)
    *pxHigherPriorityTaskWoken = pdFALSE; // no real scheduler on host to wake
}

// pdMS_TO_TICKS / vTaskDelay — Phase I Task 10 prereq cleanup replaced
// Arduino's blocking delay(ms) call sites with the native FreeRTOS
// equivalent (project_config.h §NN). There is no real scheduler on host
// builds to block, so vTaskDelay is a no-op here — matches the prior
// delay(uint32_t) no-op mock in time_mock.h.
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
inline void vTaskDelay(TickType_t /*xTicksToDelay*/) {}
