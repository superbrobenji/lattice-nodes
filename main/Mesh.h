#ifndef MESH_H
#define MESH_H

#include <functional>
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include "Adapter.h"

struct mesh_message {
  uint8_t originMacAddress[6];
  adapter_types dataType;
  uint8_t data[12];
};

class Mesh {
private:
  static Mesh* instance;

  // Broadcast MAC address (replace with your receiver MAC)
  static uint8_t broadcastAddress[6];
  uint8_t deviceMacAddress[6];

  esp_now_peer_info_t peerInfo;

  void readMacAddress();
  void printMac(const uint8_t mac[6]);
  void printMeshMessage(const mesh_message& msg);

  static void onDataSentCallback(const uint8_t* mac_addr, esp_now_send_status_t status);
  void onDataRecvCallback(const esp_now_recv_info* mac, const uint8_t* incomingData, int len);
  static void dataRecvTrampoline(const esp_now_recv_info* mac_addr, const uint8_t* data, int len);
  void transmitCore(const adapter_types type, const uint8_t data[12]);


  std::function<void(mesh_message)> externalRecvCallback;

public:
  Mesh();
  void init();
  static void transmit(const adapter_types type, const uint8_t data[12]);

  void linkDataRecvCallback(const std::function<void(mesh_message)> recvCallback);
};

#endif // MESH_H
