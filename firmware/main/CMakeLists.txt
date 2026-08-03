# ESP-IDF component CMakeLists.txt for lattice-nodes main component
#
# arduino-as-component: `espressif/arduino-esp32` (v3.x) is pulled in by
# idf_component.yml and registers the ESP-IDF component name `arduino-esp32`.
# Listing it in REQUIRES exposes Arduino.h, Serial, WiFi, String, Preferences,
# esp_now, and the rest of the Arduino API headers.
# CONFIG_AUTOSTART_ARDUINO=y (sdkconfig.defaults) provides app_main().
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
    "src/mesh/Enrollment.cpp"
    "src/mesh/PeerRegistry.cpp"
    "src/mesh/serialization/mesh.pb.c"
    "src/mesh/serialization/nanopb/pb_encode.c"
    "src/mesh/serialization/nanopb/pb_decode.c"
    "src/mesh/serialization/nanopb/pb_common.c"
    "src/persistence/EepromManager.cpp"
  INCLUDE_DIRS
    "."
    "src"
    "lib"
  REQUIRES
    arduino-esp32
    esp_wifi
    nvs_flash
    driver
    mbedtls
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
)
