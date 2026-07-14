#include "NodeContext.h"
#include <cstring>
#include "Arduino.h"
#include "esp_wifi_mock.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/pir/PirAdapter.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/persistence/EepromManager.h"
#include "src/error/ErrorCore.h"

namespace sim {

template <typename T>
static void loadImage(T& obj, std::vector<uint8_t>& image) {
  if (image.size() == sizeof(T)) memcpy(reinterpret_cast<void*>(&obj), image.data(), sizeof(T));
}
template <typename T>
static void saveImage(T& obj, std::vector<uint8_t>& image) {
  image.resize(sizeof(T));
  memcpy(image.data(), reinterpret_cast<void*>(&obj), sizeof(T));
}

void swapIn(NodeContext& ctx) {
  memcpy(EEPROM._data.data(), ctx.eepromData.data(), 512);
  EEPROM._commitCount = ctx.eepromCommitCount;
  Serial.written = ctx.serialWritten;
  Serial.output = ctx.serialOutput;
  Serial.rxQueue = ctx.serialRx;
  espNowSentPackets = ctx.espNowSent;
  espNowRegisteredPeers = ctx.espNowPeers;
  setEspNowRecvCb(ctx.espNowRecvCb);
  memcpy(mockDeviceMac, ctx.mac, 6);
  ESP._restartRequested = ctx.espRestartRequested;
  lattice::mesh::Mesh::instance = ctx.meshInstance;
  lattice::adapter::PirAdapter::instance = ctx.pirInstance;
  lattice::adapter::SerialAdapter::lastHealthMillis = ctx.serialAdapterLastHealthMillis;
  loadImage(lattice::utils::EepromManager::getInstance(), ctx.eepromManagerImage);
  loadImage(lattice::utils::ErrorCore::getInstance(), ctx.errorCoreImage);
}

void swapOut(NodeContext& ctx) {
  memcpy(ctx.eepromData.data(), EEPROM._data.data(), 512);
  ctx.eepromCommitCount = EEPROM._commitCount;
  ctx.serialWritten = Serial.written;
  ctx.serialOutput = Serial.output;
  ctx.serialRx = Serial.rxQueue;
  ctx.espNowSent = espNowSentPackets;
  ctx.espNowPeers = espNowRegisteredPeers;
  ctx.espNowRecvCb = getEspNowRecvCb();
  memcpy(ctx.mac, mockDeviceMac, 6);
  ctx.espRestartRequested = ESP._restartRequested;
  ctx.meshInstance = lattice::mesh::Mesh::instance;
  ctx.pirInstance = lattice::adapter::PirAdapter::instance;
  ctx.serialAdapterLastHealthMillis = lattice::adapter::SerialAdapter::lastHealthMillis;
  saveImage(lattice::utils::EepromManager::getInstance(), ctx.eepromManagerImage);
  saveImage(lattice::utils::ErrorCore::getInstance(), ctx.errorCoreImage);
}

} // namespace sim
