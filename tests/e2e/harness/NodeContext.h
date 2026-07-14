#pragma once
#include <array>
#include <deque>
#include <string>
#include <vector>
#include <cstdint>
#include "EEPROM.h"
#include "serial_mock.h"
#include "esp_now_mock.h"

namespace lattice {
namespace mesh {
class Mesh;
}
} // namespace lattice
namespace lattice {
namespace adapter {
class PirAdapter;
}
} // namespace lattice

namespace sim {

// One node's snapshot of every mutable global in the mock layer + firmware statics.
// swapIn() loads it into the globals; swapOut() captures the globals back.
struct NodeContext {
  // Mock layer
  std::array<uint8_t, 512> eepromData;
  int eepromCommitCount = 0;
  std::vector<uint8_t> serialWritten;
  std::string serialOutput;
  std::deque<uint8_t> serialRx;
  std::vector<EspNowSend> espNowSent;
  std::vector<esp_now_peer_info_t> espNowPeers;
  EspNowRecvCb espNowRecvCb = nullptr;
  uint8_t mac[6] = {};
  bool espRestartRequested = false;
  // Firmware statics
  lattice::mesh::Mesh* meshInstance = nullptr;
  lattice::adapter::PirAdapter* pirInstance = nullptr;
  uint32_t serialAdapterLastHealthMillis = 0;
  // Singleton object byte-images (EepromManager, ErrorCore hold per-node flags/pointers;
  // copy ctors are deleted so we snapshot raw bytes — states are flat PODs + raw pointers)
  std::vector<uint8_t> eepromManagerImage;
  std::vector<uint8_t> errorCoreImage;

  NodeContext() { eepromData.fill(0xFF); }
};

void swapIn(NodeContext& ctx);
void swapOut(NodeContext& ctx);

} // namespace sim
