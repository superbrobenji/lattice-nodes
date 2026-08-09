#include "src/mesh/Mesh.h"
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/logging/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/hardware/output/SevenSegDisplay.h"
#include "src/hardware/input/Button.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "src/persistence/eeprom/EepromCore.h"
#include "src/persistence/eeprom/EepromIdentity.h"
#include "src/persistence/eeprom/EepromRole.h"
#include "src/persistence/eeprom/EepromPeers.h"
#include "src/app/BootManager.h"
#include "src/app/DisplayManager.h"
#include "src/app/ButtonHandler.h"
#include "project_config.h"
#include <esp_wifi.h>
#include <memory>
#include <cstdio>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <nvs_flash.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Phase I Task 10 (item AAA): CONFIG_AUTOSTART_ARDUINO is gone — this is now a
// plain ESP-IDF app_main(), not an Arduino .ino. There is no more setup()/
// loop(): app_main() runs the boot sequence once (below) and returns; the
// FreeRTOS scheduler is what keeps the firmware alive from then on, driven by
// the two static tasks spawned at the end of app_main() (the Task 9
// mesh-drain task and the new housekeeping task, which absorbs everything
// that used to run every iteration of the Arduino loop() task).
//
// Logger.cpp is the one deliberately-kept Arduino API user in this codebase
// (see non-goals — it shares UART_NUM_0's Arduino Serial wrapper with
// SerialAdapter's native uart_read_bytes/uart_write_bytes path, and esp_log on
// the same UART would corrupt the binary framing SerialAdapter depends on).
// Because CONFIG_AUTOSTART_ARDUINO=y used to be what called arduino-esp32's
// initArduino() before setup(), owning app_main() ourselves means we must call
// initArduino() explicitly before Serial.begin() below, or HardwareSerial's
// internal state (which Logger.cpp's Serial.print/println/vprintf calls rely
// on) is never brought up. This is the one remaining place in this file that
// still needs Arduino.h -- transitively included via Logger.h/AdapterFactory
// -> Adapter -> project_config.h no longer pulls it in (all narrowed to
// <cstdint> in this task), so Logger.h is the sole surviving source.
extern "C" void initArduino();

using namespace lattice::utils;
// Avoid 'mesh' ambiguity by not importing the namespace
using namespace lattice::adapter;
using namespace lattice::hardware;

// Pins from config
constexpr int RED_LED_PIN = lattice::config::RED_LED_PIN;
constexpr int GREEN_LED_PIN = lattice::config::GREEN_LED_PIN;
constexpr int CONFIG_BUTTON_PIN = lattice::config::CONFIG_BUTTON_PIN;
constexpr int RESET_BUTTON_PIN = lattice::config::RESET_BUTTON_PIN;

// Compile-time dev flag
constexpr bool DEV_MODE = lattice::config::DEV_MODE;

Led greenLed(GREEN_LED_PIN);
Led redLed(RED_LED_PIN);
Button configButton(CONFIG_BUTTON_PIN);
Button resetButton(RESET_BUTTON_PIN); // New reset button object

SevenSegDisplay sevenSeg(lattice::config::SEVSEG_DATA_PIN, lattice::config::SEVSEG_CLK_PIN);

lattice::mesh::Mesh mesh;
lattice::mesh::mesh_message transmissionMessage;

std::unique_ptr<lattice::adapter::Adapter> adapter;

bool isDevMode = false;                                   // Global variable to track dev mode state
bool devMasterFlag = lattice::config::DEFAULT_DEV_MASTER; // runtime master flag used in dev mode

// define all known MAC addresses for your mesh (update with your real MACs!)
const uint8_t (*defaultPeerList)[6] = lattice::config::DEFAULT_PEERS;
constexpr int NUM_DEFAULT_PEERS = lattice::config::NUM_DEFAULT_PEERS;

// Phase I Task 9 (item EE): dedicated mesh-drain task. Static stack + TCB
// (xTaskCreateStaticPinnedToCore below) — no heap allocation, per this task's
// "no new dynamic alloc" constraint. Woken via xTaskNotifyWait() by
// onDataRecvCallback's ISR trampoline (MeshTransport.cpp) after it enqueues
// into recvQueue, instead of loop() polling for work every tick — this is what
// lets the FreeRTOS idle task (and therefore tickless idle / light sleep)
// actually go idle between mesh RX events.
static StackType_t mesh_task_stack[4096];
static StaticTask_t mesh_task_tcb;
static TaskHandle_t mesh_task_handle = nullptr;

extern "C" void mesh_task_fn(void*) {
  for (;;) {
    xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY);
    lattice::mesh::Mesh* instance = lattice::mesh::Mesh::getInstance();
    if (instance) {
      instance->drain();
    }
  }
}

// Phase I Task 10 (item AAA): housekeeping task — replaces the Arduino
// loop() task entirely. Runs everything loop() used to run every iteration
// EXCEPT the mesh recvQueue drain (Task 9 already moved that to
// mesh_task_fn's own notify-driven task above; Mesh::loop() below no longer
// includes that drain, only its other periodic work — EEPROM flush,
// enrollment-relay drain, route-report timer, deferred beacon relay, master
// beacon). Static stack + TCB, same no-heap-allocation pattern as the mesh
// task. Runs at 100 Hz (vTaskDelay(pdMS_TO_TICKS(10))) — a real yield, unlike
// the old loop()'s trailing delay(1), which let the CPU coast for at most
// ~1ms between iterations regardless of whether anything was due (flagged as
// the dominant cap on achievable light-sleep depth in the Task 9 report's
// "Concerns" #1 — this task's 10ms cadence is a deliberate, larger yield).
static StackType_t housekeeping_stack[4096];
static StaticTask_t housekeeping_tcb;

extern "C" void housekeeping_task_fn(void*) {
  // Phase I Task 10: the old Arduino loop() task was registered with the task
  // watchdog once (setup()'s esp_task_wdt_add(nullptr)) and fed every
  // iteration via esp_task_wdt_reset(). That responsibility moves here, onto
  // this task itself, since app_main()'s own task is transient (it returns
  // and is deleted once boot completes) and can no longer be the registrant.
  esp_task_wdt_add(nullptr);

  for (;;) {
    lattice::err_core::drainPendingBlink();

    // Phase I Task 7 (WW): pump both LEDs' non-blocking pulse() state
    // machines every iteration — this is what actually advances a pattern
    // armed by pulse() (dataRecvCallback, the startup blink below,
    // err_core's signalError/drainPendingBlink, and ButtonHandler's
    // role-toggle/reset confirmations).
    uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    greenLed.update(nowMs);
    redLed.update(nowMs);
    lattice::err_core::tick();

    static bool startupBlinkDone = false;
    if (!startupBlinkDone) {
      startupBlinkDone = true;
      greenLed.pulse(2, 200, 200);
      redLed.pulse(2, 200, 200);
    }

    mesh.loop(); // periodic mesh housekeeping (recvQueue itself drains via mesh_task_fn, Task 9)

    mesh.checkMasterTimeout();

    // Display state machine: show node identity on 7-segment display
    if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
      bool enrolled = mesh.isEnrolled() || mesh.getIsMaster();
      uint8_t nodeId = lattice::eeprom::loadNodeId();
      lattice::app::DisplayManager::tick(sevenSeg, enrolled, mesh.getIsMaster(), nodeId);
    }

    // Enrollment state machine: non-master nodes that are not yet enrolled
    // broadcast their public key every 10 seconds and skip sensor data
    // forwarding until approved by the server and JOIN_ACK received.
    //
    // Phase I Task 10: unlike the old loop(), which `return`ed immediately in
    // this branch (skipping the trailing delay(1) entirely and effectively
    // busy-looping while unenrolled), this task always falls through to the
    // single vTaskDelay(pdMS_TO_TICKS(10)) at the bottom — only the
    // adapter/button work below is conditionally skipped. Same behavior for
    // everything that must keep ticking (mesh.loop(), checkMasterTimeout(),
    // DisplayManager, WDT reset), with a real yield guaranteed every pass.
    bool skipDataForwarding = mesh.tickEnrollmentBroadcast(nowMs);

    esp_task_wdt_reset();

    if (!skipDataForwarding) {
      if (adapter) {
        adapter->loop();
      }
      lattice::app::ButtonHandler::tick(configButton, resetButton, mesh, greenLed, redLed,
                                        isDevMode, devMasterFlag);
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // 100 Hz — real yield
  }
}

// Validate configuration for server communication
static inline void validateServerConfiguration() {
  // Check if this is a master node intended for server communication
  bool isMasterNode = isDevMode ? devMasterFlag : lattice::eeprom::loadMasterFlag();
  bool hasSerialAdapter =
      (adapter && adapter->getAdapterType() == lattice::adapter::adapter_types::SERIAL_ADAPTER);
  bool loggingDisabled = (lattice::config::DEFAULT_LOG_LEVEL == lattice::utils::LogLevel::LOG_NONE);

  if (isMasterNode && !hasSerialAdapter &&
      lattice::config::DEFAULT_LOG_LEVEL != lattice::utils::LogLevel::LOG_NONE) {
    // This is a potential misconfiguration - master node without serial adapter might cause issues
    Logger::logln(
        "CONFIG",
        "WARNING: Master node without SERIAL_ADAPTER may cause server communication issues",
        LogLevel::LOG_WARN);
  }

  if (hasSerialAdapter && !loggingDisabled) {
    Logger::logln(
        "CONFIG",
        "WARNING: SERIAL_ADAPTER with logging enabled will interfere with server communication",
        LogLevel::LOG_WARN);
    Logger::logln("CONFIG", "Set DEFAULT_LOG_LEVEL = LOG_NONE in project_config.h",
                  LogLevel::LOG_WARN);
  }
}

// Keep main thin; adapter handles health/config

void dataRecvCallback(const lattice::mesh::mesh_message& message) {
  Logger::logln("MESH", "Data received callback triggered", LogLevel::LOG_DEBUG);
  if (adapter) {
    adapter->onMeshData(message);
  }
  // Phase I Task 7 (WW): pulse() arms the pattern without blocking this
  // receive path (the old blink() blocked every ESP-NOW receive for ~300ms);
  // the housekeeping task's greenLed.update()/redLed.update() calls animate it.
  greenLed.pulse(2, 100, 100);
}

static void initDrivers() {
  // Phase I Task 4: nvs_flash direct — initialize the NVS partition before
  // anything touches lattice::eeprom (which opens nvs_flash handles directly
  // instead of going through Arduino's Preferences wrapper). Erase-and-retry
  // on a stale/incompatible partition (fresh flash, or an NVS layout version
  // bump), matching the standard ESP-IDF boot idiom.
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  // Phase I Task 7 (QQ + RR): bundled gpio_config_t calls replace the
  // scattered per-component pinMode() calls that used to run inside each of
  // SevenSegDisplay::init()/Button::init()/GpioInput::init()/
  // GpioOutput::init() — those now only validate the pin + set _initialized.
  // Must run before any of those init() calls below, and gpio_install_isr_
  // service() must run before PirAdapter::init() (reached via adapter->init()
  // further down) calls Pir::attachInterrupt().
  gpio_config_t outCfg = {
      .pin_bit_mask = (1ULL << RED_LED_PIN) | (1ULL << GREEN_LED_PIN) |
                      (1ULL << lattice::config::SEVSEG_DATA_PIN) |
                      (1ULL << lattice::config::SEVSEG_CLK_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&outCfg));

  // Config/reset buttons: internal pull-DOWN (matches Button::init()'s prior
  // pinMode(_pin, INPUT_PULLDOWN) — line is LOW unless actively driven HIGH).
  gpio_config_t buttonInCfg = {
      .pin_bit_mask = (1ULL << CONFIG_BUTTON_PIN) | (1ULL << RESET_BUTTON_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&buttonInCfg));

  // PIR sensor: internal pull-UP (matches GpioInput::init()'s prior
  // pinMode(_pin, INPUT_PULLUP) default). Edge-interrupt type is armed
  // separately by Pir::attachInterrupt() once PirAdapter::init() runs, below.
  gpio_config_t pirInCfg = {
      .pin_bit_mask = (1ULL << lattice::adapter::PIR_ADAPTER_DEFAULT_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&pirInCfg));

  // Phase I Task 7: ISR service uses flags=0 (no ESP_INTR_FLAG_IRAM) because
  // the PIR ISR chain (Pir::isrTrampoline -> Pir::detectMotion -> Pir::signalMotion,
  // Pir::detachInterrupt) is only partially IRAM_ATTR'd — the trampolines are,
  // but the downstream methods reach gpio_isr_handler_remove and other non-IRAM
  // code paths. DO NOT change flags to ESP_INTR_FLAG_IRAM without first
  // completing the IRAM audit on the full chain — a flash-cache-disabled window
  // (e.g. during NVS commit) would crash the ISR.
  ESP_ERROR_CHECK(gpio_install_isr_service(0));

  // Phase I Task 5: uart_driver — install the native ESP-IDF UART driver for
  // UART_NUM_0 before anything touches Serial (Logger) or SerialAdapter.
  // arduino-esp32's Serial.begin() below detects the driver is already
  // installed (uart_is_driver_installed()) and skips re-installing it,
  // calling only uart_param_config() to apply matching settings — so Logger
  // (hand-rolled Serial.print path, intentionally left alone — see
  // Logger.cpp) and SerialAdapter (uart_read_bytes/uart_write_bytes) share
  // one underlying driver instance with no conflict. Baud rate here MUST
  // match Serial.begin(115200) below.
  uart_config_t uartCfg = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0)); // RX 1024, TX unbuffered
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uartCfg));

  // Phase I Task 10: with CONFIG_AUTOSTART_ARDUINO gone, nothing calls
  // arduino-esp32's initArduino() for us anymore — do it explicitly so
  // Logger.cpp's Serial.print/println/vprintf calls have a working
  // HardwareSerial underneath. Must run before Serial.begin() below.
  initArduino();
  Serial.begin(115200);
}

// Legitimate special case: the red LED itself just failed to initialize, so
// the only way to signal the failure is to try the green one and pump its
// non-blocking pulse() state machine inline (there is no other task running
// yet to do it). If neither LED works, halt silently.
static void haltOnRedLedFailure(lattice::hardware::Led& greenLed) {
  Logger::logln("MAIN", "FATAL: Failed to initialize red LED!", LogLevel::LOG_ERROR);
  if (greenLed.init()) {
    while (true) {
      greenLed.pulse(6, 100, 100);
      while (greenLed.isBusy()) {
        uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        greenLed.update(nowMs);
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } else {
    Logger::logln("MAIN", "FATAL: No LEDs available. System halted.", LogLevel::LOG_ERROR);
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void initHardwareOutputs() {
  Logger::setLogLevel(lattice::config::DEFAULT_LOG_LEVEL);

  // Phase I Task 10: was a raw, manually-gated Serial.println("Lattice
  // Starting..."); LATTICE_LOGLN folds to nothing under LOG_NONE the same way
  // the old manual `if (DEFAULT_LOG_LEVEL != LOG_NONE)` guard did, so this is
  // behavior-preserving and removes one more direct Arduino Serial call site
  // from main.cpp — Logger.cpp remains the only file that touches Serial
  // directly.
  LATTICE_LOGLN("MAIN", "Lattice Starting...", LogLevel::LOG_INFO);

  // Check and log reset reason; escalate if WDT looping
  // Must init EEPROM before BootManager::check — saveRebootReason/saveRebootCount no-op if not
  // initialized
  lattice::eeprom::init();
  lattice::app::BootManager::check();

  Logger::logln("MAIN", "Logger initialized", LogLevel::LOG_INFO);

  Led::setSystemErrorLed(&redLed);

  if (!redLed.init()) {
    haltOnRedLedFailure(greenLed);
  }

  // Seven segment conditional init
  if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
    sevenSeg.init();
    lattice::err_core::init(&redLed, &sevenSeg);
  } else {
    lattice::err_core::init(&redLed, nullptr);
  }

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
      Logger::logln("MAIN", "FATAL: Failed to initialize green LED!", LogLevel::LOG_ERROR);
      lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                          1, "MAIN: Failed to initialize green LED");
    }
  }

  if (!configButton.init()) {
    Logger::error("Config button initialization failed!");
    lattice::err::fail(lattice::utils::ErrorType::HARDWARE_FAILURE, "Config button init failed!");
  }

  if (!resetButton.init()) {
    Logger::error("Reset button initialization failed!");
    lattice::err::fail(lattice::utils::ErrorType::HARDWARE_FAILURE, "Reset button init failed!");
  }

  // Initialize EEPROM Manager
  // (finding 18: this is the authoritative checked call — the earlier
  // lattice::eeprom::init() above is an unchecked early probe needed only so
  // BootManager::check()'s reboot-reason/-count calls aren't no-ops; init()
  // is idempotent, so calling it twice is safe.)
  if (!lattice::eeprom::init()) {
    Logger::logln("MAIN", "Failed to initialize EEPROM Manager", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::CORE, 2,
                        "EEPROM Manager init failed!");
  }
}

static bool initSubsystems() {
  // Bluetooth disabled via CONFIG_BT_ENABLED=n in sdkconfig

  // Check if we're in dev mode (compile-time constant takes precedence)
  isDevMode = DEV_MODE;
  if (!isDevMode) {
    // If not compile-time dev mode, check EEPROM
    isDevMode = lattice::eeprom::loadDevFlag();
  }

  // Phase I Task 7 (XX): String concat eliminated — Logger now takes
  // const char*, and both branches are fixed literals, so a ternary of two
  // string literals covers it with no snprintf/buffer needed.
  Logger::logln("MAIN", isDevMode ? "Running in DEV mode" : "Running in PRODUCTION mode",
                LogLevel::LOG_INFO);

  // Set dev mode in AdapterFactory and EEPROM Manager
  lattice::adapter::AdapterFactory::setDevMode(isDevMode);
  lattice::eeprom::setDevMode(isDevMode);

  // Declare peers to EEPROM (only if not in dev mode and EEPROM is empty)
  if (!isDevMode && !lattice::eeprom::hasPeers()) {
    // Write default peers to EEPROM
    lattice::eeprom::savePeerList(reinterpret_cast<const uint8_t*>(defaultPeerList),
                                  NUM_DEFAULT_PEERS);
    Logger::logln("MAIN", "Wrote default peer MACs to EEPROM.", LogLevel::LOG_INFO);
  }

  // Initialize EEPROM defaults if not set (only if not in dev mode)
  if (!isDevMode) {
    lattice::adapter::AdapterFactory::initializeDefaultsIfUnset();
  }

  // Create adapter (from EEPROM if production mode, or default if dev mode)
  if (isDevMode) {
    // In dev mode, create default adapter from config
    adapter.reset(lattice::adapter::AdapterFactory::createAdapter(
        lattice::config::DEFAULT_ADAPTER, lattice::adapter::AdapterFactory::getDefaultPinForAdapter(
                                              lattice::config::DEFAULT_ADAPTER)));
    Logger::logln("MAIN", "Created default adapter (DEV mode)", LogLevel::LOG_INFO);
  } else {
    // In production mode, create from EEPROM
    adapter.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
    Logger::logln("MAIN", "Created adapter from EEPROM (PRODUCTION mode)", LogLevel::LOG_INFO);
  }

  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                        3, "MAIN: Failed to create PIR adapter");
  }
  Logger::logln("MAIN", "Adapter created", LogLevel::LOG_INFO);

  if (!adapter->init()) {
    Logger::logln("MAIN", "Adapter failed to initialize", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                        4, "MAIN: Adapter failed to initialize");
  }
  Logger::logln("MAIN", "Adapter initialized", LogLevel::LOG_INFO);

  // Phase I Task 3 (BB + ZZ): mesh.init() below owns the raw esp_netif_init()
  // + esp_event_loop_create_default() + esp_wifi_init() +
  // esp_wifi_set_mode(WIFI_MODE_STA) + esp_wifi_start() sequence that used to
  // be WiFi.mode(WIFI_STA) (see Mesh.cpp:init()) — no separate call needed
  // here.
  if (!mesh.init()) {
    Logger::logln("MAIN", "Mesh init failed", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 1,
                        "MAIN: Mesh init failed — cannot operate without mesh");
  }

  // Phase I Task 9 (item EE): create the dedicated mesh-drain task and hand
  // its handle to Mesh so the RX-ISR trampoline (MeshTransport.cpp's
  // onDataRecvCallback) can wake it via vTaskNotifyGiveFromISR. Pinned to
  // core 0 alongside the WiFi/ESP-NOW stack that delivers the RX callback;
  // priority above tskIDLE_PRIORITY so it preempts idle immediately on
  // notify, but well below time-critical ISR-adjacent work.
  mesh_task_handle = xTaskCreateStaticPinnedToCore(
      mesh_task_fn, "mesh", sizeof(mesh_task_stack) / sizeof(StackType_t), NULL,
      tskIDLE_PRIORITY + 3, mesh_task_stack, &mesh_task_tcb, 0);
  mesh.setDrainNotifyHandle(mesh_task_handle);

  mesh.setEnrollmentRelayFn(SerialAdapter::relayEnrollmentToServer);

  // Nodes must always receive — modem sleep drops ESP-NOW packets without AP sync
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (isDevMode) {
    mesh.debugDumpRadio();
  }

  // Print device public key for provisioning (admin copies this to server).
  // Only printed when not yet enrolled — enrolled nodes have already been
  // provisioned. The private key is NEVER printed — only the public key is
  // output here.
  //
  // Phase I Task 10: was Serial.print()/Serial.print(byte, HEX) — this
  // output is provisioning protocol, not a log line, so it must NOT go
  // through LATTICE_LOG* (which folds away entirely under the production
  // LOG_NONE default). Rewritten onto the native uart_write_bytes() path
  // that SerialAdapter already uses (Task 5) — one fewer thing routed
  // through Arduino's Serial wrapper, and it shares the same
  // already-installed UART_NUM_0 driver.
  if (!mesh.isEnrolled()) {
    const uint8_t* pubKey = mesh.getDevicePublicKey();
    char pubKeyLine[16 + 64 + 2]; // "LATTICE_PUBKEY:" + 64 hex chars + "\n" + NUL
    int len = snprintf(pubKeyLine, sizeof(pubKeyLine), "LATTICE_PUBKEY:");
    for (int i = 0; i < 32 && len < static_cast<int>(sizeof(pubKeyLine)); ++i) {
      len += snprintf(pubKeyLine + len, sizeof(pubKeyLine) - len, "%02X", pubKey[i]);
    }
    if (len < static_cast<int>(sizeof(pubKeyLine))) {
      len += snprintf(pubKeyLine + len, sizeof(pubKeyLine) - len, "\n");
    }
    uart_write_bytes(UART_NUM_0, pubKeyLine, len);
    Logger::logln("MAIN", "Public key printed to serial for provisioning", LogLevel::LOG_INFO);
  }

  bool isMaster;
  if (isDevMode) {
    isMaster = devMasterFlag;
    Logger::logln("MAIN", isMaster ? "DEV mode: starting as MASTER" : "DEV mode: starting as NODE",
                  LogLevel::LOG_INFO);
  } else {
    // In production mode, load from EEPROM
    isMaster = lattice::eeprom::loadMasterFlag();
  }

  mesh.setIsMaster(isMaster);
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
  Logger::logln("MAIN", isMaster ? "Booted as: MASTER" : "Booted as: NODE", LogLevel::LOG_INFO);

  adapter->setTransmitFn(&lattice::mesh::Mesh::transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);

  return isMaster;
}

static void spawnTasks(bool isMaster) {
  // Configure task watchdog: 10-second timeout. Registration
  // (esp_task_wdt_add) happens inside housekeeping_task_fn itself, below —
  // app_main()'s own task is transient and returns once boot completes, so it
  // cannot be the long-lived registrant the old Arduino loop task used to be.
  esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = 10000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);

  // Validate configuration for potential server communication issues
  validateServerConfiguration();

  // Phase I Task 10: housekeeping task absorbs the entire old loop() body
  // (adapter->loop() + DisplayManager::tick() + ButtonHandler::tick(), plus
  // everything else loop() used to run every iteration — see
  // housekeeping_task_fn's comment above). Static stack + TCB, no heap.
  xTaskCreateStaticPinnedToCore(
      housekeeping_task_fn, "hk", sizeof(housekeeping_stack) / sizeof(StackType_t), NULL,
      tskIDLE_PRIORITY + 2, housekeeping_stack, &housekeeping_tcb, tskNO_AFFINITY);

  // Phase I Task 9 (item EE) / Task 10: esp_pm_configure() is now the LAST
  // call in app_main(), made once every subsystem (GPIO, UART, NVS, radio,
  // adapter, mesh, both tasks) is fully up, rather than "as early as
  // possible" (Task 9's original placement, right after nvs_flash_init()).
  // This also folds in what used to be the separate, later
  // `if (!isMaster) setCpuFrequencyMhz(80)` arduino-esp32 call (Phase-G-era):
  // that call is gone entirely now — a leaf simply gets a pinned 80/80 DFS
  // range instead of master's dynamic 80-240 range, in one native
  // esp_pm_configure() call instead of two (one native, one
  // arduino-esp32-internal) that could otherwise fight each other (see Task
  // 9 report's "Concerns" #2).
  esp_pm_config_t pm_cfg = {
      .max_freq_mhz = isMaster ? 240 : 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true,
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
}

extern "C" void app_main(void) {
  initDrivers();
  initHardwareOutputs();
  bool isMaster = initSubsystems();
  spawnTasks(isMaster);
  // app_main() returns here; the FreeRTOS scheduler owns the runtime from
  // this point on, via the mesh-drain task and housekeeping task spawned
  // above.
}
