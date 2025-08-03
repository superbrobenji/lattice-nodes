#define DEBUG
#include "Mesh.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"
#include <esp_wifi.h>

namespace planetopia {
namespace mesh {

using namespace planetopia::utils;

Mesh *Mesh::instance = nullptr;
uint8_t Mesh::broadcastAddress[] = { 0xEC, 0x64, 0xC9, 0x5D, 0xAC, 0x18 };

Mesh::Mesh() {}

void Mesh::printMeshMessage(const mesh_message &msg) {
#ifdef DEBUG
  String macStr;
  for (int i = 0; i < 6; i++) {
    if (msg.originMacAddress[i] < 0x10) macStr += "0";
    macStr += String(msg.originMacAddress[i], HEX);
    if (i < 5) macStr += ":";
  }
  Logger::logln("MESH", "MAC: " + macStr, LogLevel::LOG_DEBUG);

  Logger::logln("MESH", "Data Type: " + String(msg.dataType), LogLevel::LOG_DEBUG);

  String dataStr;
  for (int i = 0; i < 12; i++) {
    if (msg.data[i] < 0x10) dataStr += "0";
    dataStr += String(msg.data[i], HEX) + " ";
  }
  Logger::logln("MESH", "Data: " + dataStr, LogLevel::LOG_DEBUG);
#endif
}

void Mesh::onDataSentCallback(const wifi_tx_info_t *mac_addr, esp_now_send_status_t status) {
#ifdef DEBUG
  String statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "Delivery Success" : "Delivery Fail";
  Logger::logln("MESH", "Last Packet Send Status: " + statusStr, LogLevel::LOG_DEBUG);
#endif
}

void Mesh::onDataRecvCallback(const esp_now_recv_info *mac, const uint8_t *incomingData, int len) {
  if (!incomingData || len < sizeof(mesh_message)) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      "MESH: Invalid incoming data or length");
    return;
  }

  mesh_message dataToReceive;
  memcpy(&dataToReceive, incomingData, sizeof(dataToReceive));

  Logger::logln("MESH", "Bytes received:", LogLevel::LOG_DEBUG);
  printMeshMessage(dataToReceive);

  if (externalRecvCallback) {
    externalRecvCallback(dataToReceive);
  }
}

void Mesh::dataRecvTrampoline(const esp_now_recv_info *mac_addr, const uint8_t *data, int len) {
  if (instance) {
    instance->onDataRecvCallback(mac_addr, data, len);
  }
}

void Mesh::readMacAddress() {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, deviceMacAddress);
  if (ret != ESP_OK) {
    ErrorHandler::getInstance().signalError(
      ErrorType::HARDWARE_FAILURE,
      ("MESH: Failed to read MAC address: " + String(esp_err_to_name(ret))).c_str());
  } else {
    Logger::log("MESH", "Device MAC: ", LogLevel::LOG_DEBUG);
    printMac(deviceMacAddress);
  }
}

void Mesh::printMac(const uint8_t mac[6]) {
#ifdef DEBUG
  String macStr;
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) macStr += "0";
    macStr += String(mac[i], HEX);
    if (i < 5) macStr += ":";
  }
  Logger::logln("MESH", macStr, LogLevel::LOG_DEBUG);
#endif
}

bool Mesh::init() {
  instance = this;
  WiFi.mode(WIFI_STA);

  readMacAddress();

  esp_err_t espNowInit = esp_now_init();
  if (espNowInit != ESP_OK) {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      ("MESH: Error initializing ESP-NOW: " + String(esp_err_to_name(espNowInit))).c_str());
    return false;
  }
  Logger::logln("MESH", "ESP-NOW initialized successfully", LogLevel::LOG_INFO);

  esp_now_register_send_cb(onDataSentCallback);

  // Setup peer info for broadcast
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    esp_err_t status = esp_now_add_peer(&peerInfo);
    if (status != ESP_OK) {
      ErrorHandler::getInstance().signalError(
        ErrorType::COMMUNICATION_FAIL,
        ("MESH: Peer add failed: " + String(esp_err_to_name(status))).c_str());
      return false;
    } else {
      Logger::logln("MESH", "Peer added successfully", LogLevel::LOG_INFO);
    }
  }

  esp_now_register_recv_cb(Mesh::dataRecvTrampoline);
  return true;
}

void Mesh::transmit(const adapter_types type, const uint8_t data[12]) {
  if (instance) {
    instance->transmitCore(type, data);
  }
}

void Mesh::transmitCore(const adapter_types type, const uint8_t data[12]) {
  mesh_message msg;
  memcpy(msg.originMacAddress, deviceMacAddress, sizeof(deviceMacAddress));
  msg.dataType = type;
  memcpy(msg.data, data, sizeof(data));
  printMeshMessage(msg);

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg));
  if (result == ESP_OK) {
    Logger::logln("MESH", "Sent with success", LogLevel::LOG_DEBUG);
  } else {
    ErrorHandler::getInstance().signalError(
      ErrorType::COMMUNICATION_FAIL,
      ("MESH: Error sending the data: " + String(esp_err_to_name(result))).c_str());
  }
}

void Mesh::linkDataRecvCallback(std::function<void(mesh_message)> recvCallback) {
  externalRecvCallback = recvCallback;
}

}
}
