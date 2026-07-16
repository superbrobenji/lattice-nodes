#include "NodeContext.h"
#include <cstring>
#include <stdexcept>
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
  // A fresh NodeContext's image fields are seeded from the pristine snapshot in
  // its constructor (see initPristineImages below), so by the time swapIn() runs
  // the size should always match sizeof(T) exactly. Any mismatch — including an
  // empty vector — means a context reached swapIn() without being constructed
  // through NodeContext(), or a singleton's layout changed without updating this
  // harness. Either way, silently skipping the restore would let one node's
  // state leak into another's, defeating the isolation this harness exists for
  // — so fail loudly instead.
  if (image.size() != sizeof(T)) {
    throw std::logic_error("NodeContext: singleton image size mismatch");
  }
  memcpy(reinterpret_cast<void*>(&obj), image.data(), sizeof(T));
}
template <typename T>
static void saveImage(T& obj, std::vector<uint8_t>& image) {
  image.resize(sizeof(T));
  memcpy(image.data(), reinterpret_cast<void*>(&obj), sizeof(T));
}

// Capture the singletons' untouched state once, before any node ever runs, so a
// fresh context's first swapIn() restores pristine state instead of silently
// inheriting the previous node's. Assumes the first NodeContext is constructed
// before any node boots — true for the harness's usage (contexts are
// constructed up front, before boot() runs on any of them).
static void initPristineImages(std::vector<uint8_t>& em, std::vector<uint8_t>& ec) {
  static std::vector<uint8_t> pristineEm = [] {
    std::vector<uint8_t> v;
    saveImage(lattice::utils::EepromManager::getInstance(), v);
    return v;
  }();
  static std::vector<uint8_t> pristineEc = [] {
    std::vector<uint8_t> v;
    saveImage(lattice::utils::ErrorCore::getInstance(), v);
    return v;
  }();
  em = pristineEm;
  ec = pristineEc;
}

NodeContext::NodeContext() {
  eepromData.fill(0xFF);
  initPristineImages(eepromManagerImage, errorCoreImage);
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
