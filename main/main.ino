#define DEBUG
#include "Mesh.h"
#include "AdapterFactory.h"
#include "Logger.h"

// Pins
constexpr int redLed = 25;
constexpr int greenLed = 26;
constexpr int pirSensor = 27;
constexpr int button = 33;

Mesh mesh;
mesh_message transmissionMessage;

Adapter* adapter = nullptr;

void dataRecvCallback(mesh_message message) {
  Logger::logln("MESH", "Data received callback triggered");
  digitalWrite(greenLed, HIGH);
  delay(500);
  digitalWrite(greenLed, LOW);
}

void setup() {
  Serial.begin(115200);
  Logger::logln("MAIN", "Logger initialized");

  pinMode(redLed, OUTPUT);
  digitalWrite(redLed, LOW);

  pinMode(greenLed, OUTPUT);
  digitalWrite(greenLed, LOW);

  adapter = AdapterFactory::createAdapter(PIR_ADAPTER, pirSensor);
  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter");
    digitalWrite(redLed, HIGH); // Indicate error
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
