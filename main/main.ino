#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/utils/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/hardware/input/Button.h"
#include "src/utils/ErrorHandler.h"
#include <EEPROM.h>

constexpr int EEPROM_SIZE = 128;
constexpr int MASTER_FLAG_ADDR = 0;                        // Use address 0 for the master flag
constexpr unsigned long MASTER_BEACON_INTERVAL_MS = 2000;  // 2 seconds

using namespace planetopia::utils;
using namespace planetopia::mesh;
using namespace planetopia::adapter;
using namespace planetopia::hardware;

// Pins
constexpr int RED_LED_PIN = 33;
constexpr int GREEN_LED_PIN = 26;
constexpr int PIR_SENSOR_PIN = 27;
constexpr int CONFIG_BUTTON_PIN = 32;

constexpr unsigned long BUTTON_HOLD_TIME_MS = 5000;  // 5 seconds

Led greenLed(GREEN_LED_PIN);
Led redLed(RED_LED_PIN);
Button configButton(CONFIG_BUTTON_PIN);

Mesh mesh;
mesh_message transmissionMessage;

Adapter* adapter = nullptr;

//define all known MAC addresses for your mesh (update with your real MACs!)
const uint8_t defaultPeerList[][6] = {
  {0xEC, 0x64, 0xC9, 0x5D, 0xAC, 0x18},
  {0xEC, 0x64, 0xC9, 0x5D, 0x22, 0x20},
  // Add all known node MACs, including THIS node's MAC!
};
constexpr int NUM_DEFAULT_PEERS = sizeof(defaultPeerList) / 6 / sizeof(uint8_t);

bool eepromHasPeers() {
    EEPROM.begin(EEPROM_SIZE);
    bool hasPeers = false;
    for (int i = 0; i < NUM_DEFAULT_PEERS; ++i) {
        bool found = false;
        for (int j = 0; j < 6; ++j) {
            if (EEPROM.read(16 + i * 6 + j) != 0xFF) {
                found = true;
                break;
            }
        }
        if (found) {
            hasPeers = true;
            break;
        }
    }
    EEPROM.end();
    return hasPeers;
}

void writeDefaultPeersToEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    // Wipe first
    for (int i = 0; i < MAX_PEERS * 6; ++i) {
        EEPROM.write(16 + i, 0xFF);
    }
    for (int i = 0; i < NUM_DEFAULT_PEERS; ++i) {
        for (int j = 0; j < 6; ++j) {
            EEPROM.write(16 + i * 6 + j, defaultPeerList[i][j]);
        }
    }
    EEPROM.commit();
    EEPROM.end();
}

bool loadMasterFlagFromEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    uint8_t flag = EEPROM.read(MASTER_FLAG_ADDR);
    EEPROM.end();
    return (flag == 1);  // 1 = master, 0 = not master
}

void saveMasterFlagToEEPROM(bool isMaster) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(MASTER_FLAG_ADDR, isMaster ? 1 : 0);
    EEPROM.commit();
    EEPROM.end();
}

void dataRecvCallback(mesh_message message) {
    Logger::logln("MESH", "Data received callback triggered", LogLevel::LOG_DEBUG);
    greenLed.blink(2, 100, 100);
}

void setup() {
    Serial.begin(115200);
    Logger::setLogLevel(LogLevel::LOG_DEBUG);  // Set global log level at boot

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

    ErrorHandler::getInstance().init(&redLed);

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

    // Declare peers to EEPROM (only if empty)
    if (!eepromHasPeers()) {
        writeDefaultPeersToEEPROM();
        Logger::logln("MAIN", "Wrote default peer MACs to EEPROM.", LogLevel::LOG_INFO);
    }

    adapter = AdapterFactory::createAdapter(PIR_ADAPTER, PIR_SENSOR_PIN);
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
        Logger::logln("MESH", "Mesh failed to initialize", LogLevel::LOG_ERROR);
        ErrorHandler::getInstance().signalError(
            ErrorType::COMMUNICATION_FAIL,
            "MAIN: Mesh failed to initialize");
        while (true) {
            redLed.blink(3, 150, 150);
            delay(800);
        }
    }
    bool isMaster = loadMasterFlagFromEEPROM();
    mesh.setIsMaster(isMaster);
    Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
    Logger::logln("MAIN", String("Booted as: ") + (isMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);

    adapter->setTransmitFn(mesh.transmit);

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

    if (configButton.isPressed()) {
        if (!buttonWasPressed) {
            // Just started pressing
            buttonWasPressed = true;
            holdStart = millis();
        } else {
            // Already holding, check duration
            if (millis() - holdStart >= BUTTON_HOLD_TIME_MS) {
                // Toggle master flag!
                bool wasMaster = loadMasterFlagFromEEPROM();
                bool newMaster = !wasMaster;
                saveMasterFlagToEEPROM(newMaster);
                Logger::logln("MAIN", String("Button held 5s: CONFIG TOGGLED. Now ") + (newMaster ? "MASTER" : "NODE"), LogLevel::LOG_INFO);
                Logger::logln("MAIN", "Restarting in 2 seconds for new role...", LogLevel::LOG_INFO);
                if (newMaster){
                    greenLed.blink(3, 200, 200);
                } else {
                    greenLed.blink(2, 200, 200);
                }
                delay(2000);
                ESP.restart();
            }
        }
    } else {
        buttonWasPressed = false;  // Reset state if released
    }
}
