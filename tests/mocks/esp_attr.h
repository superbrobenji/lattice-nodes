#pragma once
// Mock esp_attr.h — shadows the ESP-IDF esp_common component header for host
// tests.
//
// Phase I Task 7: production code (Pir.h, Mesh.h, PirAdapter.h) now includes
// <esp_attr.h> directly (native IDF, no longer routed through arduino-esp32's
// umbrella header) to get IRAM_ATTR on the ISR trampoline chain. These
// section-placement attributes only matter for real flash/IRAM linking, so on
// host builds they're no-ops — same treatment tests/mocks/Arduino.h already
// gives IRAM_ATTR for the arduino-esp32 include path.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
