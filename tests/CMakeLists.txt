cmake_minimum_required(VERSION 3.16)
project(planetopia_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Mock layer + firmware source include paths
# Mocks shadow real hardware headers via include order
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../main
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/Mesh/serialization/nanopb
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/src/Mesh/serialization
)

# UNIT_TEST enables friend declarations in Mesh.h for test fixture access
add_compile_definitions(UNIT_TEST)

# Firmware source files compilable on host (hardware-independent or mockable)
set(FIRMWARE_SOURCES
  ../main/src/Mesh/serialization/mesh.pb.c
  ../main/src/Mesh/serialization/nanopb/pb_encode.c
  ../main/src/Mesh/serialization/nanopb/pb_decode.c
  ../main/src/Mesh/serialization/nanopb/pb_common.c
  ../main/src/persistence/EEPROM_Manager.cpp
  ../main/src/Adapter/AdapterFactory.cpp
  ../main/src/Adapter/Adapter.cpp
  ../main/src/Adapter/Serial_Adapter/Serial_Adapter.cpp
  ../main/src/Adapter/PIR_Adapter/PIR_Adapter.cpp
  ../main/src/core/Logger.cpp
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
  mocks/firmware_stubs.cpp
  mocks/mesh_logic_impl.cpp
)

enable_testing()
include(GoogleTest)

# Helper macro — one test executable per file
macro(add_unit_test name source)
  add_executable(${name} ${source} ${FIRMWARE_SOURCES})
  target_link_libraries(${name} gtest_main gmock)
  gtest_discover_tests(${name})
endmacro()

add_unit_test(test_smoke              unit/test_smoke.cpp)
add_unit_test(test_eeprom_manager     unit/test_eeprom_manager.cpp)
add_unit_test(test_serial_framing     unit/test_serial_framing.cpp)
add_unit_test(test_replay_cache       unit/test_replay_cache.cpp)
add_unit_test(test_mesh_logic         unit/test_mesh_logic.cpp)
add_unit_test(test_pir_adapter        unit/test_pir_adapter.cpp)
