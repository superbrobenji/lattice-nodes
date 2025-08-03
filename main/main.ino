#define DEBUG
#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/utils/Logger.h"
#include "src/hardware/output/Led.h"
#include "src/utils/ErrorHandler.h"

using namespace planetopia::utils;
using namespace planetopia::mesh;
using namespace planetopia::adapter;
using namespace planetopia::hardware;

// Pins
constexpr int redLedPin = 25;
constexpr int greenLedPin = 26;
constexpr int pirSensorPin = 27;
constexpr int buttonPin = 33;

Led greenLed(greenLedPin);
Led redLed(redLedPin);

Mesh mesh;
mesh_message transmissionMessage;

Adapter* adapter = nullptr;

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

  adapter = AdapterFactory::createAdapter(PIR_ADAPTER, pirSensorPin);
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
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);

  adapter->setTransmitFn(mesh.transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);
}

void loop() {
  if (adapter) {
    adapter->loop();
  }
}
