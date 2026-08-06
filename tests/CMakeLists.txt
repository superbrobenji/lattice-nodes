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

# libsodium — real X25519/ChaCha20-Poly1305/HKDF-SHA256/HMAC-SHA256 for
# Enrollment/MeshCrypto/E2ECrypto/RouteMac on host (Phase I Task 2, item GG —
# was mbedtls, FetchContent'd above until this swap). libsodium doesn't ship a
# CMake package config, so locate it via pkg-config if available, else fall
# back to common install prefixes (Homebrew on macOS — both Apple Silicon
# /opt/homebrew and Intel /usr/local — and system paths on Linux CI). If
# neither finds it, build from source instead (same GIT_TAG the ESP-IDF side
# consumes via the espressif/libsodium component-manager package, so host and
# target link the same libsodium release).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SODIUM QUIET libsodium)
endif()
if(NOT SODIUM_FOUND)
  find_path(SODIUM_INCLUDE_DIRS NAMES sodium.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(SODIUM_LIBRARY_FILE NAMES sodium HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  if(SODIUM_INCLUDE_DIRS AND SODIUM_LIBRARY_FILE)
    set(SODIUM_LIBRARIES ${SODIUM_LIBRARY_FILE})
  endif()
endif()
if(SODIUM_INCLUDE_DIRS AND SODIUM_LIBRARIES)
  include_directories(${SODIUM_INCLUDE_DIRS})
  add_library(sodium INTERFACE)
  target_link_libraries(sodium INTERFACE ${SODIUM_LIBRARIES})
else()
  # No system libsodium found — build the same release ESP-IDF's
  # espressif/libsodium component vendors (1.0.19+) from source.
  FetchContent_Declare(
    libsodium_src
    GIT_REPOSITORY https://github.com/jedisct1/libsodium
    GIT_TAG 1.0.19
  )
  FetchContent_GetProperties(libsodium_src)
  if(NOT libsodium_src_POPULATED)
    FetchContent_Populate(libsodium_src)
  endif()
  # libsodium ships an autotools build, not CMake — build via its configure
  # script at CMake-configure time (host builds only run this once, cached).
  execute_process(
    COMMAND sh -c "cd ${libsodium_src_SOURCE_DIR} && ./configure --disable-shared --enable-static --prefix=${libsodium_src_BINARY_DIR}/install >${libsodium_src_BINARY_DIR}/configure.log 2>&1 && make -j2 install >>${libsodium_src_BINARY_DIR}/configure.log 2>&1"
    RESULT_VARIABLE SODIUM_BUILD_RESULT
  )
  if(NOT SODIUM_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "libsodium source build failed — see ${libsodium_src_BINARY_DIR}/configure.log")
  endif()
  include_directories(${libsodium_src_BINARY_DIR}/install/include)
  add_library(sodium INTERFACE)
  target_link_libraries(sodium INTERFACE ${libsodium_src_BINARY_DIR}/install/lib/libsodium.a)
endif()

# Mock layer + firmware source include paths
# Mocks shadow real hardware headers via include order — tests/mocks/ MUST
# precede firmware/main/config/ so mocks/master_pubkey_pin.h wins over the
# firmware .h.example placeholder via master_pubkey_pin_wrapper.h's
# __has_include("master_pubkey_pin.h") lookup.
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src/mesh/serialization/nanopb
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/src/mesh/serialization
)

# UNIT_TEST enables friend declarations in Mesh.h for test fixture access.
# LATTICE_ALLOW_EXAMPLE_PIN lets master_pubkey_pin_wrapper.h fall back to the
# placeholder/mock pin header on host builds (see include_directories above);
# applies to every target below (add_unit_test macro targets + lattice_e2e).
add_compile_definitions(UNIT_TEST LATTICE_ALLOW_EXAMPLE_PIN=1)

# Firmware source files compilable on host (hardware-independent or mockable)
set(FIRMWARE_SOURCES
  ../firmware/main/src/mesh/serialization/mesh.pb.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_encode.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_decode.c
  ../firmware/main/src/mesh/serialization/nanopb/pb_common.c
  ../firmware/main/src/persistence/EepromManager.cpp
  ../firmware/main/src/adapter/AdapterFactory.cpp
  ../firmware/main/src/adapter/Adapter.cpp
  ../firmware/main/src/adapter/serial/SerialFraming.cpp
  ../firmware/main/src/adapter/serial/SerialAdapter.cpp
  ../firmware/main/src/adapter/pir/PirAdapter.cpp
  ../firmware/main/src/logging/Logger.cpp
  ../firmware/main/src/error/ErrorCore.cpp
  ../firmware/main/src/hardware/output/GpioOutput.cpp
  ../firmware/main/src/hardware/output/Led.cpp
  ../firmware/main/src/hardware/output/SevenSegDisplay.cpp
  ../firmware/main/src/hardware/input/GpioInput.cpp
  ../firmware/main/src/hardware/input/Pir.cpp
  ../firmware/main/src/hardware/input/Button.cpp
  mocks/esp_now_mock.cpp
  mocks/esp_wifi_mock.cpp
  mocks/time_mock.cpp
  mocks/serial_mock.cpp
  mocks/Arduino.cpp
  mocks/EEPROM.cpp
  mocks/nvs_flash_mock.cpp
  mocks/WiFi.cpp
  ../firmware/main/src/mesh/PeerRegistry.cpp
  ../firmware/main/src/mesh/Mesh.cpp
  ../firmware/main/src/mesh/Enrollment.cpp
  ../firmware/main/src/mesh/CompactMessage.cpp
)

enable_testing()
include(GoogleTest)

# Helper macro — one test executable per file
macro(add_unit_test name source)
  add_executable(${name} ${source} ${FIRMWARE_SOURCES})
  target_link_libraries(${name} gtest_main gmock sodium)
  gtest_discover_tests(${name})
endmacro()

add_unit_test(test_smoke              unit/test_smoke.cpp)
add_unit_test(test_eeprom_manager     unit/test_eeprom_manager.cpp)
add_unit_test(test_serial_framing     unit/test_serial_framing.cpp)
add_unit_test(test_replay_cache       unit/test_replay_cache.cpp)
add_unit_test(test_mesh_logic         unit/test_mesh_logic.cpp)
add_unit_test(test_pir_adapter        unit/test_pir_adapter.cpp)
add_unit_test(test_route_report       unit/test_route_report.cpp)
add_unit_test(test_route_mac          unit/test_route_mac.cpp)
add_unit_test(test_mocks              unit/test_mocks.cpp)
add_unit_test(test_e2e_crypto         unit/test_e2e_crypto.cpp)
add_unit_test(test_e2e_keystore       unit/test_e2e_keystore.cpp)
add_unit_test(test_neighbor_table     unit/test_neighbor_table.cpp)
add_unit_test(test_route_table        unit/test_route_table.cpp)
add_unit_test(test_mac_table          unit/test_mac_table.cpp)
add_unit_test(test_mem                unit/test_mem.cpp)
add_unit_test(test_compact_message    unit/test_compact_message.cpp)
add_unit_test(test_button             unit/test_button.cpp)
add_unit_test(test_display_manager    unit/test_display_manager.cpp)
add_unit_test(test_libsodium_kat      unit/test_libsodium_kat.cpp)

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
  e2e/scenarios/test_e2e_aead.cpp
  e2e/scenarios/test_seq_wrap.cpp
  e2e/harness/NodeContext.cpp
  e2e/harness/SimNode.cpp
  e2e/harness/VirtualBus.cpp
  e2e/harness/SimWorld.cpp
  e2e/harness/FakeHub.cpp
  ${FIRMWARE_SOURCES}
)
target_compile_definitions(lattice_e2e PRIVATE SIMULATE_MODE=1)
target_include_directories(lattice_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_link_libraries(lattice_e2e gtest_main gmock sodium)
gtest_discover_tests(lattice_e2e PROPERTIES LABELS e2e)
