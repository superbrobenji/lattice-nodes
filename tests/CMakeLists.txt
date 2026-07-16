cmake_minimum_required(VERSION 3.16)
project(lattice_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# mbedtls — real Curve25519 for Enrollment/MeshCrypto on host
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)      # mbedtls' own tests
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
# Newer AppleClang enables -Wunterminated-string-initialization, which mbedtls's
# TLS layer (not needed here — only mbedcrypto/Curve25519 is used) trips on.
# We don't build/link the TLS or x509 libraries, but MBEDTLS_FATAL_WARNINGS
# still promotes that warning to -Werror while compiling them, so disable it.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(mbedtls)

# Mock layer + firmware source include paths
# Mocks shadow real hardware headers via include order
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../main
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/mesh/serialization/nanopb
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/mesh/serialization
)

# UNIT_TEST enables friend declarations in Mesh.h for test fixture access
add_compile_definitions(UNIT_TEST)

# Firmware source files compilable on host (hardware-independent or mockable)
set(FIRMWARE_SOURCES
  ../main/src/mesh/serialization/mesh.pb.c
  ../main/src/mesh/serialization/nanopb/pb_encode.c
  ../main/src/mesh/serialization/nanopb/pb_decode.c
  ../main/src/mesh/serialization/nanopb/pb_common.c
  ../main/src/persistence/EepromManager.cpp
  ../main/src/adapter/AdapterFactory.cpp
  ../main/src/adapter/Adapter.cpp
  ../main/src/adapter/serial/SerialFraming.cpp
  ../main/src/adapter/serial/SerialAdapter.cpp
  ../main/src/adapter/pir/PirAdapter.cpp
  ../main/src/logging/Logger.cpp
  ../main/src/error/ErrorCore.cpp
  ../main/src/hardware/output/GpioOutput.cpp
  ../main/src/hardware/output/Led.cpp
  ../main/src/hardware/output/SevenSegDisplay.cpp
  ../main/src/hardware/input/GpioInput.cpp
  ../main/src/hardware/input/Pir.cpp
  mocks/esp_now_mock.cpp
  mocks/esp_wifi_mock.cpp
  mocks/time_mock.cpp
  mocks/serial_mock.cpp
  mocks/Arduino.cpp
  mocks/EEPROM.cpp
  mocks/WiFi.cpp
  ../main/src/mesh/PeerRegistry.cpp
  ../main/src/mesh/Mesh.cpp
  ../main/src/mesh/Enrollment.cpp
)

enable_testing()
include(GoogleTest)

# Helper macro — one test executable per file
macro(add_unit_test name source)
  add_executable(${name} ${source} ${FIRMWARE_SOURCES})
  target_link_libraries(${name} gtest_main gmock mbedcrypto)
  gtest_discover_tests(${name})
endmacro()

add_unit_test(test_smoke              unit/test_smoke.cpp)
add_unit_test(test_eeprom_manager     unit/test_eeprom_manager.cpp)
add_unit_test(test_serial_framing     unit/test_serial_framing.cpp)
add_unit_test(test_replay_cache       unit/test_replay_cache.cpp)
add_unit_test(test_mesh_logic         unit/test_mesh_logic.cpp)
add_unit_test(test_pir_adapter        unit/test_pir_adapter.cpp)
add_unit_test(test_route_report       unit/test_route_report.cpp)
add_unit_test(test_mocks              unit/test_mocks.cpp)

# ---- E2E simulation target — SIMULATE_MODE recompiles all firmware sources ----
add_executable(lattice_e2e
  e2e/scenarios/test_harness_smoke.cpp
  e2e/scenarios/test_master_health_e2e.cpp
  e2e/scenarios/test_enrollment_e2e.cpp
  e2e/scenarios/test_pir_dataflow_e2e.cpp
  e2e/scenarios/test_multihop_e2e.cpp
  e2e/scenarios/test_route_report_e2e.cpp
  e2e/scenarios/test_replay_e2e.cpp
  e2e/scenarios/test_dual_master_e2e.cpp
  e2e/scenarios/test_hotswap_e2e.cpp
  e2e/scenarios/test_pir_health_e2e.cpp
  e2e/scenarios/test_serial_robustness_e2e.cpp
  e2e/harness/NodeContext.cpp
  e2e/harness/SimNode.cpp
  e2e/harness/VirtualBus.cpp
  e2e/harness/SimWorld.cpp
  e2e/harness/FakeHub.cpp
  ${FIRMWARE_SOURCES}
)
target_compile_definitions(lattice_e2e PRIVATE SIMULATE_MODE=1)
target_include_directories(lattice_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_link_libraries(lattice_e2e gtest_main gmock mbedcrypto)
gtest_discover_tests(lattice_e2e PROPERTIES LABELS e2e)
