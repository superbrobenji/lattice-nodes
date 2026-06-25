#include "Serial_Adapter.h"
#include "src/Adapter/AdapterFactory.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"
#include <esp_wifi.h>
#include "src/Mesh/Mesh.h"
#include <cstring>
#include <cstdio>
#if SIMULATE_MODE
#include "src/Adapter/PIR_Adapter/PIR_Adapter.h"
#endif

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

uint32_t Serial_Adapter::lastHealthMillis = 0;

static void readOwnMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

void Serial_Adapter::sendHealthReport() {
  Logger::logln("Serial_Adapter", "Sending health report", LogLevel::LOG_INFO);

  uint8_t data[12] = { 0 };
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

  Logger::logln("Serial_Adapter", String("Health report - MAC: ") + String(mac[0], HEX) + ":" + String(mac[1], HEX) + ":" + String(mac[2], HEX) + ":" + String(mac[3], HEX) + ":" + String(mac[4], HEX) + ":" + String(mac[5], HEX) + " Uptime: " + String(uptimeSec) + "s", LogLevel::LOG_DEBUG);

  if (planetopia::mesh::Mesh::transmit) {
    planetopia::mesh::Mesh::transmit(adapter_types::SERIAL_ADAPTER, data);
    Logger::logln("Serial_Adapter", "Health report sent via mesh", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Mesh transmit function not available for health report", LogLevel::LOG_WARN);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         1,
                         "Serial_Adapter: Mesh transmit not available");
  }
}

Serial_Adapter::Serial_Adapter(int pin)
  : Adapter(pin), frameState(FrameState::AwaitingLen1), frameLength(0), frameIndex(0), lastReportedHopCount(0) {
  _adapterType = adapter_types::SERIAL_ADAPTER;
  memset(payloadBuffer, 0, sizeof(payloadBuffer));

  Logger::logln("Serial_Adapter", "Serial_Adapter constructed with pin " + String(pin), LogLevel::LOG_INFO);
}

bool Serial_Adapter::init() {
  // Serial already initialized in main. Nothing to do.
  Logger::logln("Serial_Adapter", "Serial_Adapter initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void Serial_Adapter::loop() {
  // Health report: send periodically every 30s, or immediately on hop count change
  planetopia::mesh::Mesh* meshPtr = planetopia::mesh::Mesh::getInstance();
  uint32_t currentHopCount = meshPtr ? meshPtr->getHopCount() : 0;
  bool stateChanged = (currentHopCount != lastReportedHopCount);

  if (stateChanged || millis() - lastHealthMillis >= planetopia::config::HEALTH_REPORT_INTERVAL_MS) {
    lastHealthMillis = static_cast<uint32_t>(millis());
    Logger::logln("Serial_Adapter", String(stateChanged ? "Health report triggered by state change (hopCount: " : "Sending periodic health report (hopCount: ") + String(currentHopCount) + ")", LogLevel::LOG_DEBUG);
    sendHealthReport();
    lastReportedHopCount = currentHopCount;
  }

  while (Serial.available() > 0) {
    uint8_t byteIn = static_cast<uint8_t>(Serial.read());

    switch (frameState) {
      case FrameState::AwaitingLen1:
        frameLength = byteIn;
        frameState = FrameState::AwaitingLen2;
        break;

      case FrameState::AwaitingLen2:
        frameLength |= static_cast<uint16_t>(byteIn) << 8;

        if (frameLength == 0 || frameLength > MAX_PAYLOAD) {
          Logger::logln("SERIAL", "Frame parse error", LogLevel::LOG_WARN);
          planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                               planetopia::core::ModuleDigit::ADAPTER,
                               2,
                               "Serial_Adapter: Invalid frame length");
          // Reset on invalid length
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
        } else {
          frameIndex = 0;
          frameState = FrameState::AwaitingPayload;
        }
        break;

      case FrameState::AwaitingPayload:
        if (frameIndex >= MAX_PAYLOAD) {
          Logger::logln("SERIAL", "Frame parse error", LogLevel::LOG_WARN);
          planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                               planetopia::core::ModuleDigit::ADAPTER,
                               3,
                               "Serial_Adapter: Frame buffer overflow");
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
          break;
        }

        payloadBuffer[frameIndex++] = byteIn;

        if (frameIndex >= frameLength) {
          LOG_D("SERIAL", "Frame complete %u bytes", frameLength);
          handleCompleteFrame(payloadBuffer, frameLength);
          frameState = FrameState::AwaitingLen1;
          frameLength = 0;
          frameIndex = 0;
        }
        break;
    }
  }
}

void Serial_Adapter::onMeshDataImpl(const planetopia::mesh::mesh_message& message) {
  Logger::logln("Serial_Adapter", "Processing incoming mesh message - Type: " + String((uint8_t)message.messageType) + " DataType: " + String(static_cast<int32_t>(message.dataType)) + " HopCount: " + String(message.hopCount), LogLevel::LOG_DEBUG);

  // Handle control opcodes received via mesh.
  // NOTE: OP_CONFIG_SET is now handled in Adapter::onMeshData() (base class) so it reaches
  // ALL node types. Only Serial_Adapter-specific opcodes remain here.
  if (message.dataType == adapter_types::SERIAL_ADAPTER) {
    uint8_t op = message.data[0];
    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request via mesh, sending health report", LogLevel::LOG_INFO);
      sendHealthReport();
    } else if (op == OP_TX_POWER_SET) {
      uint8_t presetByte = message.data[1];
      if (presetByte > 2) {
        Logger::logln("Serial_Adapter", "Invalid TX power preset from mesh, ignoring", LogLevel::LOG_WARN);
      } else {
        auto preset = static_cast<planetopia::config::TxPowerPreset>(presetByte);
        EEPROM_Manager::getInstance().saveTxPowerPreset(preset);
        esp_err_t txErr = esp_wifi_set_max_tx_power(
            static_cast<int8_t>(planetopia::config::TX_POWER_VALUES[presetByte]));
        if (txErr != ESP_OK) {
          Logger::logln("Serial_Adapter", String("TX power set failed: ") + esp_err_to_name(txErr), LogLevel::LOG_WARN);
        } else {
          Logger::logln("Serial_Adapter", "TX power preset applied from mesh", LogLevel::LOG_INFO);
        }
      }
    }
  }

  // Forward message to server via serial (existing encoding logic)
  uint8_t encoded[MAX_PAYLOAD];
  size_t n = encodeMeshMessage(message, encoded, sizeof(encoded));

  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode mesh message for serial output", LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         4,
                         "Serial_Adapter: Message encoding failed");
    return;
  }

  Logger::logln("Serial_Adapter", "Encoded mesh message to " + String(n) + " bytes, sending to serial", LogLevel::LOG_DEBUG);

  // 2-byte little-endian length prefix
  uint8_t lenLE[2] = { static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF) };
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);

  Logger::logln("Serial_Adapter", "Mesh message sent to serial successfully", LogLevel::LOG_DEBUG);
}

size_t Serial_Adapter::encodeMeshMessage(const planetopia::mesh::mesh_message& msg, uint8_t* out, size_t outCap) {
  Logger::logln("Serial_Adapter", "Encoding mesh message - Type: " + String((uint8_t)msg.messageType) + " DataType: " + String(static_cast<int32_t>(msg.dataType)) + " HopCount: " + String(msg.hopCount), LogLevel::LOG_DEBUG);

  mesh_MeshMessage pbMsg = mesh_MeshMessage_init_zero;
  pbMsg.messageType  = static_cast<uint32_t>(msg.messageType);
  pbMsg.dataType     = static_cast<int32_t>(msg.dataType);
  pbMsg.hopCount     = msg.hopCount;
  pbMsg.epochNum     = msg.epochNum;
  pbMsg.seqNum       = static_cast<uint32_t>(msg.seqNum);
  pbMsg.protoVersion = static_cast<uint32_t>(msg.protoVersion);
  memcpy(pbMsg.originMacAddress,  msg.originMacAddress,  6);
  memcpy(pbMsg.targetMacAddress,  msg.targetMacAddress,  6);
  memcpy(pbMsg.lastHopMacAddress, msg.lastHopMacAddress, 6);

  // data field: always present (12 bytes)
  pbMsg.has_data    = true;
  pbMsg.data.size   = sizeof(msg.data);
  memcpy(pbMsg.data.bytes, msg.data, sizeof(msg.data));

  // public_key: only encode for enrollment-related message types when non-zero
  if (msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_ENROLLMENT ||
      msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_JOIN_ACK) {
    bool nonZero = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollmentPublicKey[i]) { nonZero = true; break; }
    }
    if (nonZero) {
      pbMsg.has_public_key    = true;
      pbMsg.public_key.size   = 32;
      memcpy(pbMsg.public_key.bytes, msg.enrollmentPublicKey, 32);
    }
  }

  pb_ostream_t stream = pb_ostream_from_buffer(out, outCap);
  if (!pb_encode(&stream, mesh_MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter", "nanopb encode failed", LogLevel::LOG_ERROR);
    return 0;
  }

  Logger::logln("Serial_Adapter", "Successfully encoded mesh message to " + String(stream.bytes_written) + " bytes", LogLevel::LOG_DEBUG);
  return stream.bytes_written;
}

bool Serial_Adapter::decodeMeshMessage(const uint8_t* data, size_t len, planetopia::mesh::mesh_message& outMsg) {
  Logger::logln("Serial_Adapter", "Decoding protobuf message of " + String(len) + " bytes", LogLevel::LOG_DEBUG);

  memset(&outMsg, 0, sizeof(outMsg));

  mesh_MeshMessage pbMsg = mesh_MeshMessage_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, mesh_MeshMessage_fields, &pbMsg)) {
    Logger::logln("Serial_Adapter", "nanopb decode failed", LogLevel::LOG_ERROR);
    return false;
  }

  outMsg.messageType  = static_cast<planetopia::mesh::MeshMessageType>(pbMsg.messageType);
  outMsg.dataType     = static_cast<planetopia::adapter::adapter_types>(pbMsg.dataType);
  outMsg.hopCount     = static_cast<uint8_t>(pbMsg.hopCount);
  outMsg.epochNum     = pbMsg.epochNum;
  outMsg.seqNum       = static_cast<uint16_t>(pbMsg.seqNum);
  outMsg.protoVersion = static_cast<uint8_t>(pbMsg.protoVersion);
  memcpy(outMsg.targetMacAddress, pbMsg.targetMacAddress, 6);

  if (pbMsg.has_data) {
    size_t dataToCopy = pbMsg.data.size < 12u ? pbMsg.data.size : 12u;
    memcpy(outMsg.data, pbMsg.data.bytes, dataToCopy);
  }

  if (pbMsg.has_public_key) {
    memcpy(outMsg.enrollmentPublicKey, pbMsg.public_key.bytes, 32);
  }

  // For server-to-device messages (JOIN_ACK, SERIAL_CMD_BROADCAST) the MAC
  // fields on the wire are meaningful and must not be overwritten.
  // For device-originated relays (ADAPTER_DATA, MASTER_BEACON) the server
  // leaves routing fields blank, so we fill them in with our own MAC.
  if (outMsg.messageType != planetopia::mesh::MESH_TYPE_JOIN_ACK &&
      outMsg.messageType != planetopia::mesh::MESH_TYPE_SERIAL_CMD_BROADCAST) {
    readOwnMac(outMsg.originMacAddress);
    readOwnMac(outMsg.lastHopMacAddress);
  } else {
    memcpy(outMsg.originMacAddress,  pbMsg.originMacAddress,  6);
    memcpy(outMsg.lastHopMacAddress, pbMsg.lastHopMacAddress, 6);
  }

  Logger::logln("Serial_Adapter", "Successfully decoded protobuf message", LogLevel::LOG_DEBUG);
  return true;
}

void Serial_Adapter::relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]) {
  planetopia::mesh::mesh_message msg = {};
  msg.messageType  = planetopia::mesh::MeshMessageType::MESH_TYPE_ENROLLMENT;
  msg.protoVersion = 1;
  memcpy(msg.originMacAddress,    mac,    6);
  memcpy(msg.enrollmentPublicKey, pubKey, 32);

  uint8_t encoded[128];
  size_t n = encodeMeshMessage(msg, encoded, sizeof(encoded));
  if (n == 0) {
    Logger::logln("Serial_Adapter", "Failed to encode enrollment relay message", LogLevel::LOG_ERROR);
    return;
  }

  uint8_t lenLE[2] = {
    static_cast<uint8_t>(n & 0xFF),
    static_cast<uint8_t>((n >> 8) & 0xFF)
  };
  Serial.write(lenLE, 2);
  Serial.write(encoded, n);
  Logger::logln("Serial_Adapter", "Enrollment request relayed to server", LogLevel::LOG_INFO);
}

void Serial_Adapter::handleCompleteFrame(const uint8_t* data, size_t len) {
  Logger::logln("Serial_Adapter", "Handling complete frame of " + String(len) + " bytes", LogLevel::LOG_INFO);

#if SIMULATE_MODE
  if (len >= 1) {
    uint8_t op = data[0];
    if (op == OP_SIM_PIR_TRIGGER) {
      Logger::logln("SIM", "Injecting fake PIR event", LogLevel::LOG_WARN);
      planetopia::adapter::PIR_Adapter* pirAdapter = planetopia::adapter::PIR_Adapter::getInstance();
      if (pirAdapter) pirAdapter->simulateMotion();
      return;

    } else if (op == OP_SIM_FAKE_BEACON && len >= 13) {
      Logger::logln("SIM", "Injecting fake master beacon", LogLevel::LOG_WARN);
      planetopia::mesh::mesh_message fakeBeacon{};
      fakeBeacon.protoVersion = planetopia::mesh::PROTO_VERSION;
      fakeBeacon.messageType  = planetopia::mesh::MESH_TYPE_MASTER_BEACON;
      memcpy(fakeBeacon.originMacAddress, &data[1], 6);
      memcpy(&fakeBeacon.epochNum, &data[7], 4);
      memcpy(&fakeBeacon.seqNum,   &data[11], 2);
      planetopia::mesh::Mesh* meshRef = planetopia::mesh::Mesh::getInstance();
      if (meshRef) meshRef->injectReceivedMessage(fakeBeacon.originMacAddress, fakeBeacon);
      return;

    } else if (op == OP_SIM_DUMP_STATE) {
      Logger::logln("SIM", "=== Mesh State Dump ===", LogLevel::LOG_WARN);
      planetopia::mesh::Mesh* meshRef = planetopia::mesh::Mesh::getInstance();
      if (meshRef) {
        meshRef->debugDumpRadio();
        for (size_t i = 0; i < meshRef->getPeerCount(); ++i) {
          const planetopia::mesh::PeerInfo& p = meshRef->getPeerList()[i];
          Serial.printf("  Peer[%d]: %02X:%02X:%02X:%02X:%02X:%02X last=%lums\n",
            (int)i,
            p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
            (unsigned long)p.lastSeenMillis);
        }
      }
      return;
    }
  }
#endif

  planetopia::mesh::mesh_message msg;
  if (!decodeMeshMessage(data, len, msg)) {
    Logger::logln("Serial_Adapter", "Failed to decode protobuf frame", LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::COMM,
                         planetopia::core::ModuleDigit::ADAPTER,
                         5,
                         "Serial_Adapter: Failed to decode protobuf frame");
    return;
  }

  Logger::logln("Serial_Adapter", "Decoded message - Type: " + String((uint8_t)msg.messageType) + " DataType: " + String(static_cast<int32_t>(msg.dataType)), LogLevel::LOG_INFO);

  // JOIN_ACK (type=4): server responded to an enrollment request
  if (msg.messageType == planetopia::mesh::MeshMessageType::MESH_TYPE_JOIN_ACK) {
    bool hasKey = false;
    for (int i = 0; i < 32; ++i) {
      if (msg.enrollmentPublicKey[i]) { hasKey = true; break; }
    }
    if (hasKey) {
      Logger::logln("Serial_Adapter", "Server approved enrollment, registering peer", LogLevel::LOG_INFO);
      planetopia::mesh::Mesh* meshInstance = planetopia::mesh::Mesh::getInstance();
      if (meshInstance) {
        meshInstance->enrollPeer(msg.originMacAddress, msg.enrollmentPublicKey);
      }
    } else {
      Logger::logln("Serial_Adapter", "Server rejected enrollment request", LogLevel::LOG_WARN);
    }
    return;
  }

  // Only forward adapter data via mesh transmit function; routing fields are managed by Mesh
  if (msg.messageType == planetopia::mesh::MESH_TYPE_ADAPTER_DATA) {
    Logger::logln("Serial_Adapter", "Forwarding adapter data via mesh transmit", LogLevel::LOG_DEBUG);

    if (mesh_transmit_fn) {
      // Targeted send via normal mesh transmit path (to master, route onward)
      mesh_transmit_fn(msg.dataType, msg.data);
      Logger::logln("Serial_Adapter", "Adapter data forwarded successfully", LogLevel::LOG_DEBUG);
    } else {
      Logger::logln("Serial_Adapter", "transmit function not set", LogLevel::LOG_ERROR);
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                           planetopia::core::ModuleDigit::ADAPTER,
                           6,
                           "Serial_Adapter: transmit function not set");
    }
  } else if (msg.messageType == planetopia::mesh::MESH_TYPE_SERIAL_CMD_BROADCAST) {
    Logger::logln("Serial_Adapter", "Broadcasting adapter data to all peers", LogLevel::LOG_DEBUG);
    // Broadcast adapter data to all peers
    planetopia::mesh::Mesh::broadcastAdapterDataStatic(msg.dataType, msg.data);
    Logger::logln("Serial_Adapter", "Broadcast sent successfully", LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("Serial_Adapter", "Unknown message type: " + String(msg.messageType), LogLevel::LOG_WARN);
  }

  // Handle control opcodes: CONFIG_SET, HEALTH_REQ
  if (msg.dataType == adapter_types::SERIAL_ADAPTER) {
    uint8_t op = msg.data[0];
    Logger::logln("Serial_Adapter", "Processing SERIAL_ADAPTER control opcode: 0x" + String(op, HEX), LogLevel::LOG_DEBUG);

    if (op == OP_HEALTH_REQ) {
      Logger::logln("Serial_Adapter", "Received health request, sending health report", LogLevel::LOG_INFO);
      sendHealthReport();
    } else if (op == OP_CONFIG_SET) {
      Logger::logln("Serial_Adapter", "Received configuration set request", LogLevel::LOG_INFO);

      // Apply only if targeted to me (or broadcast FF:..:FF)
      uint8_t myMac[6];
      readOwnMac(myMac);
      bool allFF = true;
      for (int i = 0; i < 6; ++i)
        if (msg.targetMacAddress[i] != 0xFF) {
          allFF = false;
          break;
        }
      bool isTarget = allFF || (memcmp(msg.targetMacAddress, myMac, 6) == 0);

      if (isTarget) {
        adapter_types newType = AdapterFactory::adapterTypeFromEEPROM(msg.data[7]);
        Logger::logln("Serial_Adapter", "Configuration applies to this node, setting adapter type to: " + String(static_cast<int32_t>(newType)), LogLevel::LOG_INFO);

        planetopia::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        Logger::logln("Serial_Adapter", "Adapter type saved to EEPROM successfully", LogLevel::LOG_INFO);
        // Pin is automatically inferred from adapter type - no need to store it
        // Let main recreate adapter on next boot or we could soft-switch by signaling error-led+restart
        Logger::logln("Serial_Adapter", "Restarting device to apply new configuration", LogLevel::LOG_INFO);
        ESP.restart();
      } else {
        Logger::logln("Serial_Adapter", "Configuration not targeted to this node, ignoring", LogLevel::LOG_DEBUG);
      }
    } else if (op == OP_TX_POWER_SET) {
      Logger::logln("Serial_Adapter", "Received TX power preset update", LogLevel::LOG_INFO);
      uint8_t presetByte = msg.data[1];
      if (presetByte > 2) {
        Logger::logln("Serial_Adapter", "Invalid TX power preset, ignoring", LogLevel::LOG_WARN);
      } else {
        auto preset = static_cast<planetopia::config::TxPowerPreset>(presetByte);
        EEPROM_Manager::getInstance().saveTxPowerPreset(preset);
        esp_err_t txErr = esp_wifi_set_max_tx_power(
            static_cast<int8_t>(planetopia::config::TX_POWER_VALUES[presetByte]));
        if (txErr != ESP_OK) {
          Logger::logln("Serial_Adapter", String("TX power set failed: ") + esp_err_to_name(txErr), LogLevel::LOG_WARN);
        } else {
          Logger::logln("Serial_Adapter", "TX power preset updated", LogLevel::LOG_INFO);
        }

        // Broadcast to all enrolled nodes so entire mesh updates
        planetopia::mesh::Mesh* meshPtr = planetopia::mesh::Mesh::getInstance();
        bool isMasterNode = meshPtr && meshPtr->getIsMaster();
        if (isMasterNode) {
          uint8_t fwdData[12] = {};
          fwdData[0] = OP_TX_POWER_SET;
          fwdData[1] = presetByte;
          planetopia::mesh::Mesh::broadcastAdapterDataStatic(adapter_types::SERIAL_ADAPTER, fwdData);
          Logger::logln("Serial_Adapter", "TX power preset broadcast to mesh", LogLevel::LOG_INFO);
        }
      }
    } else {
      Logger::logln("Serial_Adapter", "Unknown SERIAL_ADAPTER opcode: 0x" + String(op, HEX), LogLevel::LOG_WARN);
    }
  }

  Logger::logln("Serial_Adapter", "Frame processing completed successfully", LogLevel::LOG_DEBUG);
}


#ifdef UNIT_TEST
bool Serial_Adapter::injectByte(uint8_t byteIn) {
  switch (frameState) {
    case FrameState::AwaitingLen1:
      frameLength = byteIn;
      frameState = FrameState::AwaitingLen2;
      break;

    case FrameState::AwaitingLen2:
      frameLength |= static_cast<uint16_t>(byteIn) << 8;
      if (frameLength == 0 || frameLength > MAX_PAYLOAD) {
        // Invalid length — reset
        frameState = FrameState::AwaitingLen1;
        frameLength = 0;
        frameIndex = 0;
      } else {
        frameIndex = 0;
        frameState = FrameState::AwaitingPayload;
      }
      break;

    case FrameState::AwaitingPayload:
      if (frameIndex >= MAX_PAYLOAD) {
        frameState = FrameState::AwaitingLen1;
        frameLength = 0;
        frameIndex = 0;
        break;
      }
      payloadBuffer[frameIndex++] = byteIn;
      if (frameIndex >= frameLength) {
        // Frame complete
        _lastCompletedOpcode = payloadBuffer[0];
        frameState = FrameState::AwaitingLen1;
        frameLength = 0;
        frameIndex = 0;
        return true;
      }
      break;
  }
  return false;
}
#endif

}  // namespace adapter
}  // namespace planetopia
