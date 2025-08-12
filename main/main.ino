#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/core/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/hardware/input/Button.h"
#include "src/core/ErrorHandler.h"
#include "src/persistence/EEPROM_Manager.h"
#include "project_config.h"
#include <esp_wifi.h>

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

#include "src/hardware/output/SevenSegDisplay.h"
using planetopia::hardware::SevenSegDisplay;

SevenSegDisplay sevenSeg(planetopia::config::SEVSEG_DATA_PIN,
                         planetopia::config::SEVSEG_CLK_PIN);

planetopia::mesh::Mesh mesh;
planetopia::mesh::mesh_message transmissionMessage;

Adapter* adapter = nullptr;
bool isDevMode = false;  // Global variable to track dev mode state
bool devMasterFlag = planetopia::config::DEFAULT_DEV_MASTER; // runtime master flag used in dev mode

//define all known MAC addresses for your mesh (update with your real MACs!)
const uint8_t (*defaultPeerList)[6] = planetopia::config::DEFAULT_PEERS;
constexpr int NUM_DEFAULT_PEERS = planetopia::config::NUM_DEFAULT_PEERS;

// --- Serial control opcodes for SERIAL_ADAPTER (data[0]) ---
static constexpr uint8_t OP_CONFIG_SET = 0xA0;     // [A0][6B targetMac][1B adapterType][1B optPin]
static constexpr uint8_t OP_HEALTH_REQ = 0xB0;     // [B0]
static constexpr uint8_t OP_HEALTH_REPORT = 0xB1;  // [B1][1B adapterType][6B mac][4B uptimeSec]

static inline void getOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

static inline void sendHealthReport() {
  uint8_t data[12] = { 0 };
  data[0] = OP_HEALTH_REPORT;
  data[1] = static_cast<uint8_t>(adapter ? adapter->getAdapterType() : UNKNOWN_ADAPTER);
  uint8_t mac[6];
  getOwnMac(mac);
  memcpy(&data[2], mac, 6);
  uint32_t uptimeSec = millis() / 1000;
  data[8] = static_cast<uint8_t>(uptimeSec & 0xFF);
  data[9] = static_cast<uint8_t>((uptimeSec >> 8) & 0xFF);
  data[10] = static_cast<uint8_t>((uptimeSec >> 16) & 0xFF);
  data[11] = static_cast<uint8_t>((uptimeSec >> 24) & 0xFF);
  planetopia::mesh::Mesh::transmit(SERIAL_ADAPTER, data);
}

// Keep main thin; adapter handles health/config

void dataRecvCallback(planetopia::mesh::mesh_message message) {
  Logger::logln("MESH", "Data received callback triggered", LogLevel::LOG_DEBUG);
  // Ensure all nodes can handle SERIAL control messages even if they don't use Serial_Adapter
  if (message.dataType == SERIAL_ADAPTER) {
    uint8_t op = message.data[0];
    if (op == OP_CONFIG_SET) {
      uint8_t myMac[6];
      getOwnMac(myMac);
      bool targetIsBroadcast = true;
      for (int i = 0; i < 6; ++i)
        if (message.data[1 + i] != 0xFF) {
          targetIsBroadcast = false;
          break;
        }
      bool isTarget = targetIsBroadcast || (memcmp(&message.data[1], myMac, 6) == 0);
      if (isTarget) {
        // Apply configuration change
        adapter_types newType = static_cast<adapter_types>(static_cast<int8_t>(message.data[7]));
        planetopia::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        // Pin is automatically inferred from adapter type - no need to store it

        // Create new adapter with correct pin for the new type
        Adapter* newAdapter = planetopia::adapter::AdapterFactory::createFromEEPROM();
        if (newAdapter) {
          if (adapter) {
            delete adapter;
            adapter = nullptr;
          }
          adapter = newAdapter;
          adapter->setTransmitFn(&planetopia::mesh::Mesh::transmit);
          if (!adapter->init()) {
            Logger::logln("MAIN", "New adapter failed to initialize", LogLevel::LOG_ERROR);
          } else {
            Logger::logln("MAIN", "Adapter switched via CONFIG_SET", LogLevel::LOG_INFO);
          }
          sendHealthReport();
        }
      }
    } else if (op == OP_HEALTH_REQ) {
      sendHealthReport();
    }
  }
  if (adapter) {
    adapter->onMeshData(message);
  }
  greenLed.blink(2, 100, 100);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Planetopia Starting...");
  Logger::setLogLevel(planetopia::config::DEFAULT_LOG_LEVEL);

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
  ErrorHandler::getInstance().init(&redLed, &sevenSeg);
} else {
  ErrorHandler::getInstance().init(&redLed, nullptr);
}

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
      Logger::logln("MAIN", "FATAL: Failed to initialize green LED!", LogLevel::LOG_ERROR);
      ErrorHandler::getInstance().signalError(
        ErrorType::HARDWARE_FAILURE,
        "MAIN: Failed to initialize green LED");
      while (true) {
        delay(1000);
      }
    }
  }

  if (!configButton.init()) {
    Logger::error("Config button initialization failed!");
    ErrorHandler::getInstance().signalError(ErrorType::HARDWARE_FAILURE, "Config button init failed!");
  }

  if (!resetButton.init()) {
    Logger::error("Reset button initialization failed!");
    ErrorHandler::getInstance().signalError(ErrorType::HARDWARE_FAILURE, "Reset button init failed!");
  }

  // Initialize EEPROM Manager
  if (!EEPROM_Manager::getInstance().init()) {
    Logger::logln("MAIN", "Failed to initialize EEPROM Manager", LogLevel::LOG_ERROR);
    ErrorHandler::getInstance().signalError(ErrorType::MEMORY_ERROR, "EEPROM Manager init failed!");
    while (true) {
      redLed.blink(4, 100, 100);
      delay(1000);
    }
  }

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
    adapter = planetopia::adapter::AdapterFactory::createAdapter(
        planetopia::config::DEFAULT_ADAPTER,
        planetopia::adapter::AdapterFactory::getDefaultPinForAdapter(planetopia::config::DEFAULT_ADAPTER));
    Logger::logln("MAIN", "Created default adapter (DEV mode)", LogLevel::LOG_INFO);
  } else {
    // In production mode, create from EEPROM
    adapter = planetopia::adapter::AdapterFactory::createFromEEPROM();
    Logger::logln("MAIN", "Created adapter from EEPROM (PRODUCTION mode)", LogLevel::LOG_INFO);
  }

  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter", LogLevel::LOG_ERROR);
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      "MAIN: Failed to create PIR adapter");
    while (true) {
      redLed.blink(3, 150, 150);
      delay(800);
    }
  }
  Logger::logln("MAIN", "Adapter created", LogLevel::LOG_INFO);

  if (!adapter->init()) {
    Logger::logln("MAIN", "Adapter failed to initialize", LogLevel::LOG_ERROR);
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      "MAIN: Adapter failed to initialize");
    while (true) {
      redLed.blink(6, 100, 100);
      delay(1000);
    }
  }
  Logger::logln("MAIN", "Adapter initialized", LogLevel::LOG_INFO);

  if (!mesh.init()) {
    Logger::logln("MAIN", "Mesh init failed", LogLevel::LOG_ERROR);
  }
  mesh.debugDumpRadio();
  bool isMaster;
  if (isDevMode) {
    isMaster = devMasterFlag;
    Logger::logln("MAIN", String("DEV mode: starting as ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
  } else {
    // In production mode, load from EEPROM
    isMaster = EEPROM_Manager::getInstance().loadMasterFlag();
  }
  mesh.setIsMaster(isMaster);
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
  Logger::logln("MAIN", String("Booted as: ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);

  adapter->setTransmitFn(&planetopia::mesh::Mesh::transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);
  greenLed.blink(2, 200, 200);
  redLed.blink(2, 200, 200);
}

void loop() {
  if (mesh.getIsMaster()) {
    mesh.broadcastMasterBeacon();
  }

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
      // Just started pressing
      buttonWasPressed = true;
      holdStart = millis();
    } else {
      // Already holding, check duration
      if (millis() - holdStart >= BUTTON_HOLD_TIME_MS) {
        // Toggle master flag!
        if (isDevMode) {
          // Toggle runtime master flag without EEPROM persistence
          bool newMaster = !mesh.getIsMaster();
          mesh.setIsMaster(newMaster);
          devMasterFlag = newMaster;
          Logger::logln("MAIN", String("DEV MODE: Role toggled. Now ") + (newMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
          if (newMaster) {
            greenLed.blink(3, 150, 150);
          } else {
            greenLed.blink(2, 150, 150);
          }
        } else {
          bool wasMaster = EEPROM_Manager::getInstance().loadMasterFlag();
          bool newMaster = !wasMaster;
          EEPROM_Manager::getInstance().saveMasterFlag(newMaster);
          Logger::logln("MAIN", String("Button held 5s: CONFIG TOGGLED. Now ") + (newMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
          Logger::logln("MAIN", "Restarting in 2 seconds for new role...", LogLevel::LOG_INFO);
          if (newMaster) {
            greenLed.blink(3, 200, 200);
          } else {
            greenLed.blink(2, 200, 200);
          }
          delay(2000);
          ESP.restart();
        }
      }
    }
  } else {
    buttonWasPressed = false;  // Reset state if released
  }

  // Reset button handling
  if (resetButton.isPressed()) {
    if (!resetButtonWasPressed) {
      // Just started pressing reset button
      resetButtonWasPressed = true;
      resetHoldStart = millis();
    } else {
      // Already holding reset button, check duration
      if (millis() - resetHoldStart >= BUTTON_HOLD_TIME_MS) {
        // Clear all EEPROM!
        Logger::logln("MAIN", "Reset button held 5s: CLEARING ALL EEPROM!", LogLevel::LOG_WARN);
        EEPROM_Manager::getInstance().clearAll();

        // Visual feedback
        redLed.blink(5, 100, 100);
        greenLed.blink(5, 100, 100);

        Logger::logln("MAIN", "EEPROM cleared. Restarting in 3 seconds...", LogLevel::LOG_INFO);
        delay(3000);
        ESP.restart();
      }
    }
  } else {
    resetButtonWasPressed = false;  // Reset state if released
  }
  // Periodic health report
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 5000) {
    lastHealth = millis();
    sendHealthReport();
  }
}
