#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/core/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/hardware/output/SevenSegDisplay.h"
#include "src/hardware/input/Button.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "src/persistence/EEPROM_Manager.h"
#include "project_config.h"
#include <esp_wifi.h>
#include <esp_bt.h>
#include <memory>
#include <esp_task_wdt.h>

constexpr unsigned long MASTER_BEACON_INTERVAL_MS = planetopia::config::MASTER_BEACON_INTERVAL_MS;

using namespace planetopia::utils;
// Avoid 'mesh' ambiguity by not importing the namespace
using namespace planetopia::adapter;
using namespace planetopia::hardware;

// Pins from config
constexpr int RED_LED_PIN = planetopia::config::RED_LED_PIN;
constexpr int GREEN_LED_PIN = planetopia::config::GREEN_LED_PIN;
constexpr int CONFIG_BUTTON_PIN = planetopia::config::CONFIG_BUTTON_PIN;
constexpr int RESET_BUTTON_PIN = planetopia::config::RESET_BUTTON_PIN;

constexpr unsigned long BUTTON_HOLD_TIME_MS = 5000;  // 5 seconds

// Compile-time dev flag
constexpr bool DEV_MODE = planetopia::config::DEV_MODE;

Led greenLed(GREEN_LED_PIN);
Led redLed(RED_LED_PIN);
Button configButton(CONFIG_BUTTON_PIN);
Button resetButton(RESET_BUTTON_PIN);  // New reset button object

SevenSegDisplay sevenSeg(planetopia::config::SEVSEG_DATA_PIN,
                         planetopia::config::SEVSEG_CLK_PIN);

planetopia::mesh::Mesh mesh;
planetopia::mesh::mesh_message transmissionMessage;

std::unique_ptr<planetopia::adapter::Adapter> adapter;
bool isDevMode = false;                                       // Global variable to track dev mode state
bool devMasterFlag = planetopia::config::DEFAULT_DEV_MASTER;  // runtime master flag used in dev mode

//define all known MAC addresses for your mesh (update with your real MACs!)
const uint8_t (*defaultPeerList)[6] = planetopia::config::DEFAULT_PEERS;
constexpr int NUM_DEFAULT_PEERS = planetopia::config::NUM_DEFAULT_PEERS;

// Validate configuration for server communication
static inline void validateServerConfiguration() {
  // Check if this is a master node intended for server communication
  bool isMasterNode = isDevMode ? devMasterFlag : EEPROM_Manager::getInstance().loadMasterFlag();
  bool hasSerialAdapter = (adapter && adapter->getAdapterType() == planetopia::adapter::adapter_types::SERIAL_ADAPTER);
  bool loggingDisabled = (planetopia::config::DEFAULT_LOG_LEVEL == planetopia::utils::LogLevel::LOG_NONE);
  
  if (isMasterNode && !hasSerialAdapter && planetopia::config::DEFAULT_LOG_LEVEL != planetopia::utils::LogLevel::LOG_NONE) {
    // This is a potential misconfiguration - master node without serial adapter might cause issues
    Logger::logln("CONFIG", "WARNING: Master node without SERIAL_ADAPTER may cause server communication issues", LogLevel::LOG_WARN);
  }
  
  if (hasSerialAdapter && !loggingDisabled) {
    Logger::logln("CONFIG", "WARNING: SERIAL_ADAPTER with logging enabled will interfere with server communication", LogLevel::LOG_WARN);
    Logger::logln("CONFIG", "Set DEFAULT_LOG_LEVEL = LOG_NONE in project_config.h", LogLevel::LOG_WARN);
  }
}

// Keep main thin; adapter handles health/config

void dataRecvCallback(planetopia::mesh::mesh_message message) {
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
  if (planetopia::config::DEFAULT_LOG_LEVEL != planetopia::utils::LogLevel::LOG_NONE) {
    Serial.println("Planetopia Starting...");
  }
  
  Logger::setLogLevel(planetopia::config::DEFAULT_LOG_LEVEL);

  // Check and log reset reason; escalate if WDT looping
  {
    esp_reset_reason_t reason = esp_reset_reason();
    EEPROM_Manager& em = EEPROM_Manager::getInstance();
    // Must init EEPROM before setDevMode() — saveRebootReason/saveRebootCount no-op in dev mode
    em.init();
    em.saveRebootReason(static_cast<uint8_t>(reason));
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
      uint8_t count = em.loadRebootCount();
      count++;
      em.saveRebootCount(count);
      Serial.printf("[BOOT] WDT reset #%d (reason: %d)\n", count, (int)reason);
      if (count >= 5) {
        Serial.println("[BOOT] WDT loop detected — halting. Manual reset required.");
        while (true) { delay(1000); }
      }
    } else {
      em.saveRebootCount(0);  // Clean boot resets the counter
    }
  }

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
  if (planetopia::config::ENABLE_SEVSEG_DISPLAY) {
    sevenSeg.init();
    planetopia::utils::ErrorCore::getInstance().init(&redLed, &sevenSeg);
  } else {
    planetopia::utils::ErrorCore::getInstance().init(&redLed, nullptr);
  }

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
      Logger::logln("MAIN", "FATAL: Failed to initialize green LED!", LogLevel::LOG_ERROR);
      planetopia::err::fatal(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::CORE,
                            1,
                            "MAIN: Failed to initialize green LED");
    }
  }

  if (!configButton.init()) {
    Logger::error("Config button initialization failed!");
    planetopia::err::fail(planetopia::utils::ErrorType::HARDWARE_FAILURE, "Config button init failed!");
  }

  if (!resetButton.init()) {
    Logger::error("Reset button initialization failed!");
    planetopia::err::fail(planetopia::utils::ErrorType::HARDWARE_FAILURE, "Reset button init failed!");
  }

  // Initialize EEPROM Manager
  if (!EEPROM_Manager::getInstance().init()) {
    Logger::logln("MAIN", "Failed to initialize EEPROM Manager", LogLevel::LOG_ERROR);
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::MEMORY,
                          planetopia::core::ModuleDigit::CORE,
                          2,
                          "EEPROM Manager init failed!");
  }

  // Disable Bluetooth — unused, saves 20-30mA
  btStop();

  // Check if we're in dev mode (compile-time constant takes precedence)
  isDevMode = DEV_MODE;
  if (!isDevMode) {
    // If not compile-time dev mode, check EEPROM
    isDevMode = EEPROM_Manager::getInstance().loadDevFlag();
  }

  Logger::logln("MAIN", String("Running in ") + (isDevMode ? "DEV" : "PRODUCTION") + " mode", LogLevel::LOG_INFO);

  // Set dev mode in AdapterFactory and EEPROM Manager
  planetopia::adapter::AdapterFactory::setDevMode(isDevMode);
  EEPROM_Manager::getInstance().setDevMode(isDevMode);

  // Declare peers to EEPROM (only if not in dev mode and EEPROM is empty)
  if (!isDevMode && !EEPROM_Manager::getInstance().hasPeers()) {
    // Write default peers to EEPROM
    EEPROM_Manager::getInstance().savePeerList(
      reinterpret_cast<const uint8_t*>(defaultPeerList),
      NUM_DEFAULT_PEERS);
    Logger::logln("MAIN", "Wrote default peer MACs to EEPROM.", LogLevel::LOG_INFO);
  }

  // Initialize EEPROM defaults if not set (only if not in dev mode)
  if (!isDevMode) {
    planetopia::adapter::AdapterFactory::initializeDefaultsIfUnset();
  }

  // Create adapter (from EEPROM if production mode, or default if dev mode)
  if (isDevMode) {
    // In dev mode, create default adapter from config
    adapter.reset(planetopia::adapter::AdapterFactory::createAdapter(
      planetopia::config::DEFAULT_ADAPTER,
      planetopia::adapter::AdapterFactory::getDefaultPinForAdapter(planetopia::config::DEFAULT_ADAPTER)));
    Logger::logln("MAIN", "Created default adapter (DEV mode)", LogLevel::LOG_INFO);
  } else {
    // In production mode, create from EEPROM
    adapter.reset(planetopia::adapter::AdapterFactory::createFromEEPROM());
    Logger::logln("MAIN", "Created adapter from EEPROM (PRODUCTION mode)", LogLevel::LOG_INFO);
  }

  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter", LogLevel::LOG_ERROR);
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::HARDWARE,
                          planetopia::core::ModuleDigit::CORE,
                          3,
                          "MAIN: Failed to create PIR adapter");
  }
  Logger::logln("MAIN", "Adapter created", LogLevel::LOG_INFO);

  if (!adapter->init()) {
    Logger::logln("MAIN", "Adapter failed to initialize", LogLevel::LOG_ERROR);
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::HARDWARE,
                          planetopia::core::ModuleDigit::CORE,
                          4,
                          "MAIN: Adapter failed to initialize");
  }
  Logger::logln("MAIN", "Adapter initialized", LogLevel::LOG_INFO);

  if (!mesh.init()) {
    Logger::logln("MAIN", "Mesh init failed", LogLevel::LOG_ERROR);
    planetopia::err::fatal(planetopia::core::ErrorTypeDigit::COMM,
                          planetopia::core::ModuleDigit::MESH,
                          1,
                          "MAIN: Mesh init failed — cannot operate without mesh");
  }

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
    Serial.print("PLANETOPIA_PUBKEY:");
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
    isMaster = EEPROM_Manager::getInstance().loadMasterFlag();
  }

  // Master keeps 240MHz for serial USB reliability
  // Sensor nodes: 80MHz sufficient for I/O-bound relay work, saves ~30% CPU power
  if (!isMaster) {
    setCpuFrequencyMhz(80);
  }

  mesh.setIsMaster(isMaster);
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
  Logger::logln("MAIN", String("Booted as: ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);

  adapter->setTransmitFn(&planetopia::mesh::Mesh::transmit);

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
  planetopia::utils::ErrorCore::getInstance().drainPendingBlink();

  static bool startupBlinkDone = false;
  if (!startupBlinkDone) {
    startupBlinkDone = true;
    greenLed.blink(2, 200, 200);
    redLed.blink(2, 200, 200);
  }

  mesh.loop();  // drains recv queue, ticks beacon (if master)

  mesh.checkMasterTimeout();

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

  static bool buttonWasPressed = false;
  static unsigned long holdStart = 0;

  // Reset button logic
  static bool resetButtonWasPressed = false;
  static unsigned long resetHoldStart = 0;

  if (configButton.isPressed()) {
    if (!buttonWasPressed) {
      buttonWasPressed = true;
      holdStart = millis();
    } else if (millis() - holdStart >= BUTTON_HOLD_TIME_MS) {
      buttonWasPressed = false;  // Reset BEFORE action to prevent re-fire
      if (isDevMode) {
        bool newMaster = !mesh.getIsMaster();
        mesh.setIsMaster(newMaster);
        devMasterFlag = newMaster;
        Logger::logln("MAIN", String("DEV MODE: Role toggled. Now ") + (newMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
        greenLed.blink(newMaster ? 3 : 2, 150, 150);
      } else {
        bool wasMaster = EEPROM_Manager::getInstance().loadMasterFlag();
        bool newMaster = !wasMaster;
        EEPROM_Manager::getInstance().saveMasterFlag(newMaster);
        Logger::logln("MAIN", String("Button held 5s: CONFIG TOGGLED. Now ") + (newMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
        Logger::logln("MAIN", "Restarting in 2 seconds for new role...", LogLevel::LOG_INFO);
        greenLed.blink(newMaster ? 3 : 2, 200, 200);
        delay(2000);
        EEPROM_Manager::getInstance().forceFlush();
        ESP.restart();
      }
    }
  } else {
    buttonWasPressed = false;
  }

  static bool resetConfirmPending = false;
  static uint32_t resetConfirmDeadline = 0;

  if (resetButton.isPressed()) {
    if (!resetButtonWasPressed) {
      resetButtonWasPressed = true;
      resetHoldStart = millis();
    } else if (millis() - resetHoldStart >= BUTTON_HOLD_TIME_MS) {
      resetButtonWasPressed = false;
      if (!resetConfirmPending) {
        resetConfirmPending = true;
        resetConfirmDeadline = millis() + 3000;
        Logger::logln("MAIN", "Reset armed: hold again within 3s to confirm EEPROM wipe", LogLevel::LOG_WARN);
        redLed.blink(3, 100, 100);
      } else if (millis() < resetConfirmDeadline) {
        resetConfirmPending = false;
        Logger::logln("MAIN", "EEPROM wipe confirmed. Clearing all...", LogLevel::LOG_WARN);
        EEPROM_Manager::getInstance().clearAll();
        redLed.blink(5, 100, 100);
        greenLed.blink(5, 100, 100);
        delay(3000);
        EEPROM_Manager::getInstance().forceFlush();
        ESP.restart();
      }
    }
  } else {
    resetButtonWasPressed = false;
    if (resetConfirmPending && millis() > resetConfirmDeadline) {
      resetConfirmPending = false;
      Logger::logln("MAIN", "Reset confirmation timed out", LogLevel::LOG_INFO);
    }
  }
  // REMOVED: periodic health report was here.
  // Serial_Adapter::loop() handles this correctly when adapter type is SERIAL_ADAPTER.

  delay(1);  // Yield to FreeRTOS idle task — allows CPU power gating between iterations
}
