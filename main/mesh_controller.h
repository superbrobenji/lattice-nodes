#ifndef MESH_CONTROLLER_H
#define MESH_CONTROLLER_H
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>

enum messageTypes {
  PIR_SENSOR_MESSAGE_TYPE,
  WIFI_MESSAGE_TYPE,
  LED_MESSAGE_TYPE,
};

typedef struct mesh_message {
  uint8_t originMacAddress[6];
  enum messageTypes dataType;
  byte data[12];
} mesh_message;

void setupMesh();
void transmitData(mesh_message dataToTransmit);

#endif