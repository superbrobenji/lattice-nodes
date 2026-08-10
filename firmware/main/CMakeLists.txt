# ESP-IDF component CMakeLists.txt for lattice-nodes main component
#
# Phase C finding 17: the `arduino-esp32` component (and its
# idf_component.yml dependency) was removed here once Logger.cpp — the last
# file in the tree using Arduino.h/Serial — migrated to native
# uart_write_bytes(), the same path SerialAdapter already used. Nothing in
# this component graph depends on Arduino anymore.
# Persistence (src/persistence/eeprom/*) uses the `nvs_flash` component
# directly (nvs_open/nvs_get_*/nvs_set_*) rather than Arduino's Preferences
# wrapper — Phase I Task 4.
#
# Phase C final review: dropping arduino-esp32 above also dropped its
# transitive exposure of esp_timer.h (main.cpp/Mesh.h/PirAdapter.cpp call
# esp_timer_get_time() directly but never declared `esp_timer` themselves —
# arduino-esp32 was silently providing it). `esp_wifi` and `driver` both
# depend on esp_timer too, but only via PRIV_REQUIRES, which ESP-IDF's build
# system does not propagate to consumers — hence the explicit REQUIRES entry
# below. esp_pm.h (main.cpp's esp_pm_configure()) and esp_task_wdt.h
# (main.cpp's esp_task_wdt_*()) were checked too: esp_pm.h stays resolvable
# because `driver`'s own CMakeLists.txt REQUIRES esp_pm publicly (so it
# propagates transitively through `driver` above), and esp_task_wdt.h lives
# in the esp_system component, which ESP-IDF's build system adds to every
# component's requirements automatically (build.cmake's
# __COMPONENT_REQUIRES_COMMON) — neither needed an explicit entry here.
idf_component_register(
  SRCS
    "main.cpp"
    "src/adapter/Adapter.cpp"
    "src/adapter/AdapterFactory.cpp"
    "src/adapter/pir/PirAdapter.cpp"
    "src/adapter/serial/SerialAdapter.cpp"
    "src/adapter/serial/SerialFraming.cpp"
    "src/error/ErrorCore.cpp"
    "src/hardware/input/Button.cpp"
    "src/hardware/input/GpioInput.cpp"
    "src/hardware/input/Pir.cpp"
    "src/hardware/output/GpioOutput.cpp"
    "src/hardware/output/Led.cpp"
    "src/hardware/output/SevenSegDisplay.cpp"
    "src/logging/Logger.cpp"
    "src/mesh/Mesh.cpp"
    "src/mesh/MeshTransport.cpp"
    "src/mesh/MasterBeacon.cpp"
    "src/mesh/DownlinkRouter.cpp"
    "src/mesh/Enrollment.cpp"
    "src/mesh/PendingRelayQueue.cpp"
    "src/mesh/PeerRegistry.cpp"
    "src/mesh/PeerEnrollment.cpp"
    "src/mesh/UplinkRouter.cpp"
    "src/mesh/MeshMessenger.cpp"
    "src/mesh/RouteReportHandler.cpp"
    "src/mesh/FrameAuthorizer.cpp"
    "src/mesh/serialization/mesh.pb.c"
    "src/mesh/serialization/nanopb/pb_encode.c"
    "src/mesh/serialization/nanopb/pb_decode.c"
    "src/mesh/serialization/nanopb/pb_common.c"
    "src/persistence/eeprom/EepromCore.cpp"
    "src/persistence/eeprom/EepromIdentity.cpp"
    "src/persistence/eeprom/EepromRole.cpp"
    "src/persistence/eeprom/EepromSecurity.cpp"
    "src/persistence/eeprom/EepromPeers.cpp"
    "src/persistence/eeprom/EepromDiagnostics.cpp"
    "src/persistence/eeprom/EepromEnrollment.cpp"
    "src/persistence/eeprom/EepromDeviceConfig.cpp"
  INCLUDE_DIRS
    "."
    "src"
    "lib"
  REQUIRES
    esp_wifi
    nvs_flash
    driver
    mbedtls
    esp_timer
)

# Subdirectory includes for IDE/IntelliSense and internal #include resolution
target_include_directories(${COMPONENT_LIB} PUBLIC
  src/adapter
  src/adapter/pir
  src/adapter/serial
  src/app
  src/error
  src/hardware
  src/hardware/input
  src/hardware/output
  src/logging
  src/mesh
  src/mesh/serialization
  src/mesh/serialization/nanopb
  src/network
  src/persistence
  src/persistence/eeprom
)
