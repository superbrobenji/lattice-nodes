#define DEBUG
#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/utils/Logger.h"
#include "src/hardware/Led.h"
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
  Logger::logln("MESH", "Data received callback triggered");
  greenLed.toggle();
}

void setup() {
  Serial.begin(115200);
  Logger::logln("MAIN", "Logger initialized");

  Led::setSystemErrorLed(&redLed);

  // Try to initialize red LED. If it fails, fallback to Serial log and green LED.
  if (!redLed.init()) {
    Logger::logln("MAIN", "FATAL: Failed to initialize red LED!");
    // Try to use green LED if possible
    if (greenLed.init()) {
      while (true) {
        greenLed.blink(6, 100, 100);  // 6 quick blinks: fatal hardware error
        delay(1000);
      }
    } else {
      Logger::logln("MAIN", "FATAL: No LEDs available. System halted.");
      while (true) {
        delay(1000);
      }
    }
  }

  ErrorHandler::getInstance().init(&redLed);

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
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
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      "MAIN: Failed to create PIR adapter");
    while (true) {
      redLed.blink(3, 150, 150);
      delay(800);
    }
  }
  Logger::logln("MAIN", "Adapter created");

  if (!adapter->init()) {
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      "MAIN: Adapter failed to initialize");
    while (true) {
      redLed.blink(6, 100, 100);
      delay(1000);
    }
  }
  Logger::logln("MAIN", "Adapter initialized");

  if (!mesh.init()) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      "MAIN: Mesh failed to initialize");
    while (true) {
      redLed.blink(3, 150, 150);
      delay(800);
    }
  }
  Logger::logln("MESH", "Mesh initialized");
  adapter->setTransmitFn(mesh.transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);
}

void loop() {
  if (adapter) {
    adapter->loop();
  }
}
