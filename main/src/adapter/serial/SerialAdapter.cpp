#include "SerialAdapter.h"
#include "src/adapter/AdapterFactory.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <esp_wifi.h>
#include "src/mesh/Mesh.h"
#include <cstring>
#include <cstdio>
#if SIMULATE_MODE
#include "src/adapter/pir/PirAdapter.h"
#endif

namespace lattice {
namespace adapter {

using namespace lattice::utils;

uint32_t SerialAdapter::lastHealthMillis = 0;

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

void SerialAdapter::sendHealthReport() {
  Logger::logln("Serial_Adapter", "Sending health report", LogLevel::LOG_INFO);

  uint8_t data[64] = {0};
  data[0] = OP_HEALTH_REPORT;
  data[1] = AdapterFactory::adapterTypeToEEPROM(adapter_types::SERIAL_ADAPTER);

  uint8_t mac[6];
  readOwnMac(mac);
  memcpy(&data[2], mac, 6);

  uint32_t uptimeSec = millis() / 1000;
  data[8] = static_cast<uint8_t>(uptimeSec & 0xFF);
  data[9] = static_cast<uint8_t>((uptimeSec >> 8) & 0xFF);
  data[10] = static_cast<uint8_t>((uptimeSec >> 16) & 0xFF);
  data[11] = static_cast<uint8_t>((uptimeSec >> 24) & 0xFF);

  Logger::logln("Serial_Adapter",
                String("Health report - MAC: ") + String(mac[0], HEX) + ":" + String(mac[1], HEX) +
                    ":" + String(mac[2], HEX) + ":" + String(mac[3], HEX) + ":" +
                    String(mac[4], HEX) + ":" + String(mac[5], HEX) +
                    " Uptime: " + String(uptimeSec) + "s",
                LogLevel::LOG_DEBUG);

  if (lattice::mesh::Mesh::transmit) {
    lattice::mesh::Mesh::transmit(adapter_types::SERIAL_ADAPTER, data);
    Logger::logln("Serial_Adapter", "Health report sent via mesh", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Mesh transmit function not available for health report",
                  LogLevel::LOG_WARN);
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER, 1,
                       "Serial_Adapter: Mesh transmit not available");
  }
}

SerialAdapter::SerialAdapter(int pin)
    : Adapter(pin), lastReportedHopCount(0) {
  _adapterType = adapter_types::SERIAL_ADAPTER;

  Logger::logln("Serial_Adapter", "Serial_Adapter constructed with pin " + String(pin),
                LogLevel::LOG_INFO);
}

bool SerialAdapter::init() {
  // Serial already initialized in main. Nothing to do.
  Logger::logln("Serial_Adapter", "Serial_Adapter initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void SerialAdapter::loop() {
  // Health report: send periodically every 30s, or immediately on hop count change
  lattice::mesh::Mesh* meshPtr = lattice::mesh::Mesh::getInstance();
  uint32_t currentHopCount = meshPtr ? meshPtr->getHopCount() : 0;
  bool stateChanged = (currentHopCount != lastReportedHopCount);

  if (stateChanged || millis() - lastHealthMillis >= lattice::config::HEALTH_REPORT_INTERVAL_MS) {
    lastHealthMillis = static_cast<uint32_t>(millis());
    Logger::logln("Serial_Adapter",
                  String(stateChanged ? "Health report triggered by state change (hopCount: "
                                      : "Sending periodic health report (hopCount: ") +
                      String(currentHopCount) + ")",
                  LogLevel::LOG_DEBUG);
    sendHealthReport();
    lastReportedHopCount = currentHopCount;
  }

  while (Serial.available() > 0) {
    uint8_t byteIn = static_cast<uint8_t>(Serial.read());
    if (_framing.injectByte(byteIn)) {
      handleCompleteFrame(_framing.frameBuffer(), _framing.frameLen());
    }
  }
}

void SerialAdapter::onMeshDataImpl(const lattice::mesh::mesh_message& message) {
  Logger::logln(
      "Serial_Adapter",
      "Processing incoming mesh message - Type: " + String((uint8_t)message.message_type) +
          " DataType: " + String(static_cast<int32_t>(message.data_type)) +
          " HopCount: " + String(message.hop_count),
      LogLevel::LOG_DEBUG);

  // Handle control opcodes received via mesh.
  // NOTE: OP_CONFIG_SET is now handled in Adapter::onMeshData() (base class) so it reaches
  // ALL node types. Only SerialAdapter-specific opcodes remain here.
  if (message.data_type == adapter_types::SERIAL_ADAPTER) {
    uint8_t op = message.data[0];
    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request via mesh, sending health report",
                    LogLevel::LOG_INFO);
      sendHealthReport();
    } else if (op == OP_TX_POWER_SET) {
      uint8_t presetByte = message.data[1];
      if (presetByte > 2) {
        Logger::logln("Serial_Adapter", "Invalid TX power preset from mesh, ignoring",
                      LogLevel::LOG_WARN);
      } else {
        auto preset = static_cast<lattice::config::TxPowerPreset>(presetByte);
        EepromManager::getInstance().saveTxPowerPreset(preset);
        esp_err_t txErr = esp_wifi_set_max_tx_power(
            static_cast<int8_t>(lattice::config::TX_POWER_VALUES[presetByte]));
        if (txErr != ESP_OK) {
          Logger::logln("Serial_Adapter", String("TX power set failed: ") + esp_err_to_name(txErr),
                        LogLevel::LOG_WARN);
        } else {
          Logger::logln("Serial_Adapter", "TX power preset applied from mesh", LogLevel::LOG_INFO);
        }
      }
    }
  }

  // Forward message to server via serial (existing encoding logic)
  uint8_t encoded[256];
  size_t n = lattice::adapter::serial::SerialFraming::encode(message, encoded, sizeof(encoded));

  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode mesh message for serial output",
                  LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER, 4,
                       "Serial_Adapter: Message encoding failed");
    return;
  }

  Logger::logln("Serial_Adapter",
                "Encoded mesh message to " + String(n) + " bytes, sending to serial",
                LogLevel::LOG_DEBUG);

  // 2-byte little-endian length prefix
  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);

  Logger::logln("Serial_Adapter", "Mesh message sent to serial successfully", LogLevel::LOG_DEBUG);
}


void SerialAdapter::relayEnrollmentToServer(const uint8_t* mac, const uint8_t* pubKey) {
  lattice::mesh::mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  msg.proto_version = 1;
  memcpy(msg.origin_mac_address, mac, 6);
  memcpy(msg.enrollment_public_key, pubKey, 32);

  uint8_t encoded[128];
  size_t n = lattice::adapter::serial::SerialFraming::encode(msg, encoded, sizeof(encoded));
  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode enrollment relay message",
                  LogLevel::LOG_ERROR);
    return;
  }

  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);
  Logger::logln("Serial_Adapter", "Enrollment request relayed to server", LogLevel::LOG_INFO);
}

void SerialAdapter::handleCompleteFrame(const uint8_t* data, size_t len) {
  Logger::logln("Serial_Adapter", "Handling complete frame of " + String(len) + " bytes",
                LogLevel::LOG_INFO);

#if SIMULATE_MODE
  if (len >= 1) {
    uint8_t op = data[0];
    if (op == OP_SIM_PIR_TRIGGER) {
      Logger::logln("SIM", "Injecting fake PIR event", LogLevel::LOG_WARN);
      lattice::adapter::PirAdapter* pirAdapter = lattice::adapter::PirAdapter::getInstance();
      if (pirAdapter)
        pirAdapter->simulateMotion();
      return;

    } else if (op == OP_SIM_FAKE_BEACON && len >= 13) {
      Logger::logln("SIM", "Injecting fake master beacon", LogLevel::LOG_WARN);
      lattice::mesh::mesh_message fakeBeacon{};
      fakeBeacon.proto_version = lattice::mesh::PROTO_VERSION;
      fakeBeacon.message_type = MESH_TYPE_MASTER_BEACON;
      memcpy(fakeBeacon.origin_mac_address, &data[1], 6);
      memcpy(&fakeBeacon.epoch_num, &data[7], 4);
      memcpy(&fakeBeacon.seq_num, &data[11], 2);
      lattice::mesh::Mesh* meshRef = lattice::mesh::Mesh::getInstance();
      if (meshRef)
        meshRef->injectReceivedMessage(fakeBeacon.origin_mac_address, fakeBeacon);
      return;

    } else if (op == OP_SIM_DUMP_STATE) {
      Logger::logln("SIM", "=== Mesh State Dump ===", LogLevel::LOG_WARN);
      lattice::mesh::Mesh* meshRef = lattice::mesh::Mesh::getInstance();
      if (meshRef) {
        meshRef->debugDumpRadio();
        for (size_t i = 0; i < meshRef->getPeerCount(); ++i) {
          const lattice::mesh::PeerInfo& p = meshRef->getPeerList()[i];
          Serial.printf("  Peer[%d]: %02X:%02X:%02X:%02X:%02X:%02X last=%lums\n", (int)i, p.mac[0],
                        p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
                        (unsigned long)p.lastSeenMillis);
        }
      }
      return;
    }
  }
#endif

  lattice::mesh::mesh_message msg;
  if (!lattice::adapter::serial::SerialFraming::decode(data, len, msg)) {
    Logger::logln("Serial_Adapter", "Failed to decode protobuf frame", LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER, 5,
                       "Serial_Adapter: Failed to decode protobuf frame");
    return;
  }

  Logger::logln("Serial_Adapter",
                "Decoded message - Type: " + String((uint8_t)msg.message_type) +
                    " DataType: " + String(static_cast<int32_t>(msg.data_type)),
                LogLevel::LOG_INFO);

  // JOIN_ACK (type=4): server responded to an enrollment request
  if (msg.message_type == MESH_TYPE_JOIN_ACK) {
    bool hasKey = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollment_public_key[i]) {
        hasKey = true;
        break;
      }
    }
    if (hasKey) {
      Logger::logln("Serial_Adapter", "Server approved enrollment, registering peer",
                    LogLevel::LOG_INFO);
      lattice::mesh::Mesh* meshInstance = lattice::mesh::Mesh::getInstance();
      if (meshInstance) {
        meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key);
      }
    } else {
      Logger::logln("Serial_Adapter", "Server rejected enrollment request", LogLevel::LOG_WARN);
    }
    return;
  }

  // Only forward adapter data via mesh transmit function; routing fields are managed by Mesh
  if (msg.message_type == MESH_TYPE_ADAPTER_DATA) {
    Logger::logln("Serial_Adapter", "Forwarding adapter data via mesh transmit",
                  LogLevel::LOG_DEBUG);

    if (mesh_transmit_fn) {
      // Targeted send via normal mesh transmit path (to master, route onward)
      mesh_transmit_fn(static_cast<adapter_types>(msg.data_type), msg.data);
      Logger::logln("Serial_Adapter", "Adapter data forwarded successfully", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("Serial_Adapter", "transmit function not set", LogLevel::LOG_ERROR);
      lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                         6, "Serial_Adapter: transmit function not set");
    }
  } else if (msg.message_type == MESH_TYPE_SERIAL_CMD_BROADCAST) {
    Logger::logln("Serial_Adapter", "Broadcasting adapter data to all peers", LogLevel::LOG_DEBUG);
    // Broadcast adapter data to all peers
    lattice::mesh::Mesh::broadcastAdapterDataStatic(static_cast<adapter_types>(msg.data_type),
                                                    msg.data);
    Logger::logln("Serial_Adapter", "Broadcast sent successfully", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Unknown message type: " + String(msg.message_type),
                  LogLevel::LOG_WARN);
  }

  // Handle control opcodes: CONFIG_SET, HEALTH_REQ
  if (msg.data_type == adapter_types::SERIAL_ADAPTER) {
    uint8_t op = msg.data[0];
    Logger::logln("Serial_Adapter",
                  "Processing SERIAL_ADAPTER control opcode: 0x" + String(op, HEX),
                  LogLevel::LOG_DEBUG);

    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request, sending health report",
                    LogLevel::LOG_INFO);
      sendHealthReport();
    } else if (op == OP_CONFIG_SET) {
      Logger::logln("Serial_Adapter", "Received configuration set request", LogLevel::LOG_INFO);

      // Apply only if targeted to me (or broadcast FF:..:FF)
      uint8_t myMac[6];
      readOwnMac(myMac);
      bool allFF = true;
      for (int i = 0; i < 6; ++i)
        if (msg.target_mac_address[i] != 0xFF) {
          allFF = false;
          break;
        }
      bool isTarget = allFF || (memcmp(msg.target_mac_address, myMac, 6) == 0);

      if (isTarget) {
        adapter_types newType = AdapterFactory::adapterTypeFromEEPROM(msg.data[7]);
        Logger::logln("Serial_Adapter",
                      "Configuration applies to this node, setting adapter type to: " +
                          String(static_cast<int32_t>(newType)),
                      LogLevel::LOG_INFO);

        lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        Logger::logln("Serial_Adapter", "Adapter type saved to EEPROM successfully",
                      LogLevel::LOG_INFO);
        // Pin is automatically inferred from adapter type - no need to store it
        // Let main recreate adapter on next boot or we could soft-switch by signaling
        // error-led+restart
        Logger::logln("Serial_Adapter", "Restarting device to apply new configuration",
                      LogLevel::LOG_INFO);
        ESP.restart();
      } else {
        Logger::logln("Serial_Adapter", "Configuration not targeted to this node, ignoring",
                      LogLevel::LOG_DEBUG);
      }
    } else if (op == OP_TX_POWER_SET) {
      Logger::logln("Serial_Adapter", "Received TX power preset update", LogLevel::LOG_INFO);
      uint8_t presetByte = msg.data[1];
      if (presetByte > 2) {
        Logger::logln("Serial_Adapter", "Invalid TX power preset, ignoring", LogLevel::LOG_WARN);
      } else {
        auto preset = static_cast<lattice::config::TxPowerPreset>(presetByte);
        EepromManager::getInstance().saveTxPowerPreset(preset);
        esp_err_t txErr = esp_wifi_set_max_tx_power(
            static_cast<int8_t>(lattice::config::TX_POWER_VALUES[presetByte]));
        if (txErr != ESP_OK) {
          Logger::logln("Serial_Adapter", String("TX power set failed: ") + esp_err_to_name(txErr),
                        LogLevel::LOG_WARN);
        } else {
          Logger::logln("Serial_Adapter", "TX power preset updated", LogLevel::LOG_INFO);
        }

        // Broadcast to all enrolled nodes so entire mesh updates
        lattice::mesh::Mesh* meshPtr = lattice::mesh::Mesh::getInstance();
        bool isMasterNode = meshPtr && meshPtr->getIsMaster();
        if (isMasterNode) {
          uint8_t fwdData[64] = {};
          fwdData[0] = OP_TX_POWER_SET;
          fwdData[1] = presetByte;
          lattice::mesh::Mesh::broadcastAdapterDataStatic(adapter_types::SERIAL_ADAPTER, fwdData);
          Logger::logln("Serial_Adapter", "TX power preset broadcast to mesh", LogLevel::LOG_INFO);
        }
      }
    } else if (op == OP_NODE_ID_SET) {
      Logger::logln("Serial_Adapter", "Received node ID set request", LogLevel::LOG_INFO);
      uint8_t myMac[6];
      readOwnMac(myMac);
      bool allFF = true;
      for (int i = 0; i < 6; ++i)
        if (msg.data[1 + i] != 0xFF) {
          allFF = false;
          break;
        }
      bool isTarget = allFF || (memcmp(&msg.data[1], myMac, 6) == 0);
      if (isTarget) {
        uint8_t nodeId = msg.data[7];
        lattice::utils::EepromManager::getInstance().saveNodeId(nodeId);
        Logger::logln("Serial_Adapter", "Node ID set: " + String(nodeId), LogLevel::LOG_INFO);
      }
    } else {
      Logger::logln("Serial_Adapter", "Unknown SERIAL_ADAPTER opcode: 0x" + String(op, HEX),
                    LogLevel::LOG_WARN);
    }
  }

  Logger::logln("Serial_Adapter", "Frame processing completed successfully", LogLevel::LOG_DEBUG);
}

} // namespace adapter
} // namespace lattice
