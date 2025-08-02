#define DEBUG
#include "src/Mesh/Mesh.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/utils/Logger.h"
#include "src/hardware/Led.h"

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

  greenLed.init();
  redLed.init();

  adapter = AdapterFactory::createAdapter(PIR_ADAPTER, pirSensorPin);
  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter");
    redLed.on(); // Indicate error
    while (true) {
      delay(1000);
    }
  }
  Logger::logln("MAIN", "Adapter created");

  adapter->init();
  Logger::logln("MAIN", "Adapter initialized");

  mesh.init();
  Logger::logln("MESH", "Mesh initialized");
  adapter->setTransmitFn(mesh.transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);
}

void loop() {
  if (adapter) {
    adapter->loop();
  }
}
