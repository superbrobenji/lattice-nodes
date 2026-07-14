#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/logging/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/hardware/output/SevenSegDisplay.h"
#include "src/hardware/input/Button.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "src/persistence/EepromManager.h"
#include "src/app/BootManager.h"
#include "src/app/DisplayManager.h"
#include "src/app/ButtonHandler.h"
#include "project_config.h"
#include <esp_wifi.h>
#include <esp_bt.h>
#include <memory>
#include <esp_task_wdt.h>

constexpr unsigned long MASTER_BEACON_INTERVAL_MS = lattice::config::MASTER_BEACON_INTERVAL_MS;

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
Button resetButton(RESET_BUTTON_PIN);  // New reset button object

SevenSegDisplay sevenSeg(lattice::config::SEVSEG_DATA_PIN,
                         lattice::config::SEVSEG_CLK_PIN);

lattice::mesh::Mesh mesh;
lattice::mesh::mesh_message transmissionMessage;

std::unique_ptr<lattice::adapter::Adapter> adapter;
bool isDevMode = false;                                       // Global variable to track dev mode state
bool devMasterFlag = lattice::config::DEFAULT_DEV_MASTER;  // runtime master flag used in dev mode

//define all known MAC addresses for your mesh (update with your real MACs!)
const uint8_t (*defaultPeerList)[6] = lattice::config::DEFAULT_PEERS;
constexpr int NUM_DEFAULT_PEERS = lattice::config::NUM_DEFAULT_PEERS;

// Validate configuration for server communication
static inline void validateServerConfiguration() {
  // Check if this is a master node intended for server communication
  bool isMasterNode = isDevMode ? devMasterFlag : EepromManager::getInstance().loadMasterFlag();
  bool hasSerialAdapter = (adapter && adapter->getAdapterType() == lattice::adapter::adapter_types::SERIAL_ADAPTER);
  bool loggingDisabled = (lattice::config::DEFAULT_LOG_LEVEL == lattice::utils::LogLevel::LOG_NONE);
  
  if (isMasterNode && !hasSerialAdapter && lattice::config::DEFAULT_LOG_LEVEL != lattice::utils::LogLevel::LOG_NONE) {
    // This is a potential misconfiguration - master node without serial adapter might cause issues
    Logger::logln("CONFIG", "WARNING: Master node without SERIAL_ADAPTER may cause server communication issues", LogLevel::LOG_WARN);
  }
  
  if (hasSerialAdapter && !loggingDisabled) {
    Logger::logln("CONFIG", "WARNING: SERIAL_ADAPTER with logging enabled will interfere with server communication", LogLevel::LOG_WARN);
    Logger::logln("CONFIG", "Set DEFAULT_LOG_LEVEL = LOG_NONE in project_config.h", LogLevel::LOG_WARN);
  }
}

// Keep main thin; adapter handles health/config

void dataRecvCallback(const lattice::mesh::mesh_message& message) {
  Logger::logln("MESH", "Data received callback triggered", LogLevel::LOG_DEBUG);
  if (adapter) {
    adapter->onMeshData(message);
  }
  greenLed.blink(2, 100, 100);
}

void setup() {
  Serial.begin(115200);
  
  // Only print startup message if logging is enabled (not LOG_NONE)
  // This prevents text output when using SERIAL_ADAPTER for server communication
  if (lattice::config::DEFAULT_LOG_LEVEL != lattice::utils::LogLevel::LOG_NONE) {
    Serial.println("Lattice Starting...");
  }
  
  Logger::setLogLevel(lattice::config::DEFAULT_LOG_LEVEL);

  // Check and log reset reason; escalate if WDT looping
  // Must init EEPROM before BootManager::check — saveRebootReason/saveRebootCount no-op if not initialized
  EepromManager::getInstance().init();
  lattice::app::BootManager::check(EepromManager::getInstance());

  Logger::logln("MAIN", "Logger initialized", LogLevel::LOG_INFO);

  Led::setSystemErrorLed(&redLed);

  if (!redLed.init()) {
    Logger::logln("MAIN", "FATAL: Failed to initialize red LED!", LogLevel::LOG_ERROR);
    if (greenLed.init()) {
      while (true) {
        greenLed.blink(6, 100, 100);
        delay(1000);
      }
    } else {
      Logger::logln("MAIN", "FATAL: No LEDs available. System halted.", LogLevel::LOG_ERROR);
      while (true) {
        delay(1000);
      }
    }
  }

  // Seven segment conditional init
  if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
    sevenSeg.init();
    lattice::utils::ErrorCore::getInstance().init(&redLed, &sevenSeg);
  } else {
    lattice::utils::ErrorCore::getInstance().init(&redLed, nullptr);
  }

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
      Logger::logln("MAIN", "FATAL: Failed to initialize green LED!", LogLevel::LOG_ERROR);
      lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE,
                            lattice::core::ModuleDigit::CORE,
                            1,
                            "MAIN: Failed to initialize green LED");
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
  if (!EepromManager::getInstance().init()) {
    Logger::logln("MAIN", "Failed to initialize EEPROM Manager", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::MEMORY,
                          lattice::core::ModuleDigit::CORE,
                          2,
                          "EEPROM Manager init failed!");
  }

  // Disable Bluetooth — unused, saves 20-30mA
  btStop();

  // Check if we're in dev mode (compile-time constant takes precedence)
  isDevMode = DEV_MODE;
  if (!isDevMode) {
    // If not compile-time dev mode, check EEPROM
    isDevMode = EepromManager::getInstance().loadDevFlag();
  }

  Logger::logln("MAIN", String("Running in ") + (isDevMode ? "DEV" : "PRODUCTION") + " mode", LogLevel::LOG_INFO);

  // Set dev mode in AdapterFactory and EEPROM Manager
  lattice::adapter::AdapterFactory::setDevMode(isDevMode);
  EepromManager::getInstance().setDevMode(isDevMode);

  // Declare peers to EEPROM (only if not in dev mode and EEPROM is empty)
  if (!isDevMode && !EepromManager::getInstance().hasPeers()) {
    // Write default peers to EEPROM
    EepromManager::getInstance().savePeerList(
      reinterpret_cast<const uint8_t*>(defaultPeerList),
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
      lattice::config::DEFAULT_ADAPTER,
      lattice::adapter::AdapterFactory::getDefaultPinForAdapter(lattice::config::DEFAULT_ADAPTER)));
    Logger::logln("MAIN", "Created default adapter (DEV mode)", LogLevel::LOG_INFO);
  } else {
    // In production mode, create from EEPROM
    adapter.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
    Logger::logln("MAIN", "Created adapter from EEPROM (PRODUCTION mode)", LogLevel::LOG_INFO);
  }

  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE,
                          lattice::core::ModuleDigit::CORE,
                          3,
                          "MAIN: Failed to create PIR adapter");
  }
  Logger::logln("MAIN", "Adapter created", LogLevel::LOG_INFO);

  if (!adapter->init()) {
    Logger::logln("MAIN", "Adapter failed to initialize", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE,
                          lattice::core::ModuleDigit::CORE,
                          4,
                          "MAIN: Adapter failed to initialize");
  }
  Logger::logln("MAIN", "Adapter initialized", LogLevel::LOG_INFO);

  if (!mesh.init()) {
    Logger::logln("MAIN", "Mesh init failed", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::COMM,
                          lattice::core::ModuleDigit::MESH,
                          1,
                          "MAIN: Mesh init failed — cannot operate without mesh");
  }

  mesh.setEnrollmentRelayFn(Serial_Adapter::relayEnrollmentToServer);

  // Nodes must always receive — modem sleep drops ESP-NOW packets without AP sync
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (isDevMode) {
    mesh.debugDumpRadio();
  }

  // Print device public key for provisioning (admin copies this to server).
  // Only printed when not yet enrolled — enrolled nodes have already been provisioned.
  // The private key is NEVER printed — only the public key is output here.
  if (!mesh.isEnrolled()) {
    const uint8_t* pubKey = mesh.getDevicePublicKey();
    Serial.print("LATTICE_PUBKEY:");
    for (int i = 0; i < 32; ++i) {
      if (pubKey[i] < 0x10) Serial.print("0");
      Serial.print(pubKey[i], HEX);
    }
    Serial.println();
    Logger::logln("MAIN", "Public key printed to serial for provisioning", LogLevel::LOG_INFO);
  }

  bool isMaster;
  if (isDevMode) {
    isMaster = devMasterFlag;
    Logger::logln("MAIN", String("DEV mode: starting as ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
  } else {
    // In production mode, load from EEPROM
    isMaster = EepromManager::getInstance().loadMasterFlag();
  }

  // Master keeps 240MHz for serial USB reliability
  // Sensor nodes: 80MHz sufficient for I/O-bound relay work, saves ~30% CPU power
  if (!isMaster) {
    setCpuFrequencyMhz(80);
  }

  mesh.setIsMaster(isMaster);
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
  Logger::logln("MAIN", String("Booted as: ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);

  adapter->setTransmitFn(&lattice::mesh::Mesh::transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);

  // Configure task watchdog: 10-second timeout
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 10000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(nullptr);  // Add current task

  // Validate configuration for potential server communication issues
  validateServerConfiguration();
  
}

void loop() {
  lattice::utils::ErrorCore::getInstance().drainPendingBlink();

  static bool startupBlinkDone = false;
  if (!startupBlinkDone) {
    startupBlinkDone = true;
    greenLed.blink(2, 200, 200);
    redLed.blink(2, 200, 200);
  }

  mesh.loop();  // drains recv queue, ticks beacon (if master)

  mesh.checkMasterTimeout();

  // Display state machine: show node identity on 7-segment display
  if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
    bool enrolled = mesh.isEnrolled() || mesh.getIsMaster();
    uint8_t nodeId = lattice::utils::EepromManager::getInstance().loadNodeId();
    lattice::app::DisplayManager::tick(sevenSeg, enrolled, mesh.getIsMaster(), nodeId);
  }

  // Enrollment state machine: non-master nodes that are not yet enrolled
  // broadcast their public key every 10 seconds and skip sensor data forwarding
  // until approved by the server and JOIN_ACK received.
  static uint32_t lastEnrollmentBroadcast = 0;
  if (!mesh.isEnrolled() && !mesh.getIsMaster()) {
    if (millis() - lastEnrollmentBroadcast > 10000) {
      lastEnrollmentBroadcast = millis();
      mesh.sendEnrollmentRequest();
      Logger::logln("MAIN", "Enrollment request sent (awaiting server approval)", LogLevel::LOG_INFO);
    }
    esp_task_wdt_reset();
    // Do not forward sensor data until enrolled
    return;
  }

  esp_task_wdt_reset();
  if (adapter) {
    adapter->loop();
  }

  lattice::app::ButtonHandler::tick(configButton, resetButton, mesh,
    lattice::utils::EepromManager::getInstance(),
    greenLed, redLed, isDevMode, devMasterFlag);
  // REMOVED: periodic health report was here.
  // Serial_Adapter::loop() handles this correctly when adapter type is SERIAL_ADAPTER.

  delay(1);  // Yield to FreeRTOS idle task — allows CPU power gating between iterations
}
