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
