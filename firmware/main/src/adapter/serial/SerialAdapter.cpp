#include "SerialAdapter.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/mesh/Mesh.h"
#include "src/network/MacEq.h"
#include <cstring>
#include <cstdio>
#include <esp_timer.h>
#if SIMULATE_MODE
#include "src/adapter/pir/PirAdapter.h"
#endif

namespace lattice {
namespace adapter {

using namespace lattice::utils;

// Phase H2 item W: frame-building + transmission now live in the shared base
// Adapter::sendSelfHealthReport() (uses Adapter::buildHealthFrame). This
// wrapper stays because loop() needs its own callsite/log line.
void SerialAdapter::sendHealthReport() {
  LATTICE_LOGLN("Serial_Adapter", "Sending health report", LogLevel::LOG_INFO);
  sendSelfHealthReport();
  LATTICE_LOGLN("Serial_Adapter", "Health report sent via mesh", LogLevel::LOG_DEBUG);
}

SerialAdapter::SerialAdapter(uint8_t pin) : Adapter(pin), lastReportedHopCount(0) {
  _adapterType = adapter_types::SERIAL_ADAPTER;

  LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_INFO, "Serial_Adapter constructed with pin %u",
               (unsigned)pin);
}

bool SerialAdapter::init() {
  // Serial already initialized in main. Nothing to do.
  LATTICE_LOGLN("Serial_Adapter", "Serial_Adapter initialized successfully", LogLevel::LOG_INFO);
  return true;
}

void SerialAdapter::loop() {
  // Health report: send periodically every 30s, or immediately on hop count change.
  // The interval tick itself (Phase H2 item W) lives in the shared Adapter
  // base — healthTickDue() both checks and (if due) resets the timer.
  lattice::mesh::Mesh* meshPtr = lattice::mesh::Mesh::getInstance();
  uint32_t currentHopCount = meshPtr ? meshPtr->getHopCount() : 0;
  bool stateChanged = (currentHopCount != lastReportedHopCount);

  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  bool intervalDue = healthTickDue(now);
  if (stateChanged || intervalDue) {
    // Interval didn't fire on its own but the hop-count change triggers a
    // report anyway — reset the shared timer so the next periodic report is
    // still a full interval out, matching the original combined check.
    if (!intervalDue)
      resetHealthTick(now);
    if (stateChanged) {
      LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_DEBUG,
                   "Health report triggered by state change (hopCount: %lu)",
                   (unsigned long)currentHopCount);
    } else {
      LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_DEBUG,
                   "Sending periodic health report (hopCount: %lu)",
                   (unsigned long)currentHopCount);
    }
    sendHealthReport();
    lastReportedHopCount = currentHopCount;
  }

  // Phase I Task 5: native uart_driver read, non-blocking (ticks_to_wait=0).
  // Driver already installed once at boot (main.cpp) — see UART_NUM_0 comment
  // in SerialAdapter.h. Loop until the RX FIFO is drained (rather than one
  // bounded read) to preserve the old Serial.available()-loop's behavior:
  // a single loop() call fully drains whatever arrived since the last call,
  // however many chunks of 64 that takes.
  uint8_t rxBuf[64];
  int rxLen;
  while ((rxLen = uart_read_bytes(_uartNum, rxBuf, sizeof(rxBuf), 0)) > 0) {
    for (int i = 0; i < rxLen; ++i) {
      if (_framing.injectByte(rxBuf[i])) {
        handleCompleteFrame(_framing.frameBuffer(), _framing.frameLen());
      }
    }
  }
}

void SerialAdapter::onMeshDataImpl(const lattice::mesh::mesh_message& message) {
  LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_DEBUG,
               "Processing incoming mesh message - Type: %u DataType: %ld HopCount: %u",
               (unsigned)message.message_type, (long)message.data_type,
               (unsigned)message.hop_count);

  // Control opcodes (CONFIG_SET/NODE_ID_SET/HEALTH_REQ/TX_POWER_SET) received
  // via mesh are all handled once, in Adapter::onMeshData() (base class),
  // before onMeshDataImpl() is ever invoked — see the shared dispatch table
  // in Adapter.h/.cpp (Phase H2 item V). Nothing left to do for them here.

  // Forward message to server via serial (existing encoding logic)
  uint8_t encoded[256];
  size_t n = lattice::adapter::serial::SerialFraming::encode(message, encoded, sizeof(encoded));

  if (n == 0) {
    LATTICE_LOGLN("Serial_Adapter", "Failed to encode mesh message for serial output",
                  LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER, 4,
                       "Serial_Adapter: Message encoding failed");
    return;
  }

  LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_DEBUG,
               "Encoded mesh message to %u bytes, sending to serial", (unsigned)n);

  // 2-byte little-endian length prefix
  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  uart_write_bytes(_uartNum, lenLE, 2);
  uart_write_bytes(_uartNum, encoded, n);

  LATTICE_LOGLN("Serial_Adapter", "Mesh message sent to serial successfully", LogLevel::LOG_DEBUG);
}

void SerialAdapter::relayEnrollmentToServer(const uint8_t* mac, const uint8_t* pubKey) {
  lattice::mesh::mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  msg.proto_version = lattice::mesh::PROTO_VERSION;
  memcpy(msg.origin_mac_address, mac, 6);
  memcpy(msg.enrollment_public_key, pubKey, 32);

  uint8_t encoded[128];
  size_t n = lattice::adapter::serial::SerialFraming::encode(msg, encoded, sizeof(encoded));
  if (n == 0) {
    LATTICE_LOGLN("Serial_Adapter", "Failed to encode enrollment relay message",
                  LogLevel::LOG_ERROR);
    return;
  }

  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  // Static method — no adapter instance, so use UART_NUM_0 directly (the
  // only UART port this firmware ever installs; see SerialAdapter.h).
  uart_write_bytes(UART_NUM_0, lenLE, 2);
  uart_write_bytes(UART_NUM_0, encoded, n);
  LATTICE_LOGLN("Serial_Adapter", "Enrollment request relayed to server", LogLevel::LOG_INFO);
}

void SerialAdapter::handleCompleteFrame(const uint8_t* data, size_t len) {
  LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_INFO, "Handling complete frame of %u bytes",
               (unsigned)len);

#if SIMULATE_MODE
  if (len >= 1) {
    uint8_t op = data[0];
    if (op == OP_SIM_PIR_TRIGGER) {
      LATTICE_LOGLN("SIM", "Injecting fake PIR event", LogLevel::LOG_WARN);
      lattice::adapter::PirAdapter* pirAdapter = lattice::adapter::PirAdapter::getInstance();
      if (pirAdapter)
        pirAdapter->simulateMotion();
      return;

    } else if (op == OP_SIM_FAKE_BEACON && len >= 13) {
      LATTICE_LOGLN("SIM", "Injecting fake master beacon", LogLevel::LOG_WARN);
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
      LATTICE_LOGLN("SIM", "=== Mesh State Dump ===", LogLevel::LOG_WARN);
      lattice::mesh::Mesh* meshRef = lattice::mesh::Mesh::getInstance();
      if (meshRef) {
        meshRef->debugDumpRadio();
        for (size_t i = 0; i < meshRef->getPeerCount(); ++i) {
          const lattice::mesh::PeerInfo& p = meshRef->getPeerList()[i];
          // Phase I Task 6 (NN): %lu -> %llu — lastSeenMs widened uint32_t ->
          // uint64_t (FF); nano-format (CONFIG_LIBC_NEWLIB_NANO_FORMAT=y)
          // requires an exact-width specifier match.
          LATTICE_LOGF("SIM", LogLevel::LOG_WARN,
                       "  Peer[%d]: %02X:%02X:%02X:%02X:%02X:%02X last=%llums", (int)i, p.mac[0],
                       p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
                       (unsigned long long)p.lastSeenMs);
        }
      }
      return;
    }
  }
#endif

  lattice::mesh::mesh_message msg;
  if (!lattice::adapter::serial::SerialFraming::decode(data, len, msg)) {
    LATTICE_LOGLN("Serial_Adapter", "Failed to decode protobuf frame", LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::ADAPTER, 5,
                       "Serial_Adapter: Failed to decode protobuf frame");
    return;
  }

  LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_INFO, "Decoded message - Type: %u DataType: %ld",
               (unsigned)msg.message_type, (long)msg.data_type);

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
      LATTICE_LOGLN("Serial_Adapter", "Server approved enrollment, registering peer",
                    LogLevel::LOG_INFO);
      lattice::mesh::Mesh* meshInstance = lattice::mesh::Mesh::getInstance();
      if (meshInstance) {
        // Server may relay a secondary-master identity alongside the
        // enrollment approval (Phase 4 dual-master failover) — pass it
        // through to the JOIN_ACK only if it's actually present (non-zero).
        // Protocol v0.6.0 (wire shrink §8): packed into data[4..42] rather
        // than top-level MeshMessage fields.
        const uint8_t* secondaryMasterMac = msg.data + 4;
        const uint8_t* secondaryPublicKey = msg.data + 10;
        bool hasSecondary = false;
        for (int i = 0; i < 6; ++i) {
          if (secondaryMasterMac[i]) {
            hasSecondary = true;
            break;
          }
        }
        if (hasSecondary) {
          meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key,
                                   secondaryMasterMac, secondaryPublicKey);
        } else {
          meshInstance->enrollPeer(msg.target_mac_address, msg.enrollment_public_key);
        }
      }
    } else {
      LATTICE_LOGLN("Serial_Adapter", "Server rejected enrollment request", LogLevel::LOG_WARN);
    }
    return;
  }

  // Only forward adapter data via mesh transmit function; routing fields are managed by Mesh
  if (msg.message_type == MESH_TYPE_ADAPTER_DATA) {
    LATTICE_LOGLN("Serial_Adapter", "Forwarding adapter data via mesh transmit",
                  LogLevel::LOG_DEBUG);

    if (mesh_transmit_fn) {
      // Targeted send via normal mesh transmit path (to master, route onward)
      mesh_transmit_fn(static_cast<adapter_types>(msg.data_type), msg.data);
      LATTICE_LOGLN("Serial_Adapter", "Adapter data forwarded successfully", LogLevel::LOG_DEBUG);
    } else {
      LATTICE_LOGLN("Serial_Adapter", "transmit function not set", LogLevel::LOG_ERROR);
      lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                         6, "Serial_Adapter: transmit function not set");
    }
  } else if (msg.message_type == MESH_TYPE_SERIAL_CMD_BROADCAST) {
    static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool isGenuineBroadcast = lattice::mac::eq(msg.target_mac_address, kBroadcastMac);
    if (isGenuineBroadcast) {
      LATTICE_LOGLN("Serial_Adapter", "Broadcasting adapter data to all peers",
                    LogLevel::LOG_DEBUG);
      // Broadcast adapter data to all peers (plaintext — no single destination to seal for)
      lattice::mesh::Mesh::broadcastAdapterDataStatic(static_cast<adapter_types>(msg.data_type),
                                                      msg.data);
      LATTICE_LOGLN("Serial_Adapter", "Broadcast sent successfully", LogLevel::LOG_DEBUG);
    } else {
      // Master -> node command: source-route + seal to the specific destination
      // (spec §4), instead of flooding it in plaintext to every peer. The
      // opcode's target field (data[1..6], e.g. CONFIG_SET/NODE_ID_SET) carries
      // the destination MAC.
      const uint8_t* fwdData = msg.data;
      uint8_t destMac[6];
      memcpy(destMac, &fwdData[1], 6); // CONFIG_SET target field
      LATTICE_LOGLN("Serial_Adapter", "Sending sealed, source-routed downlink to node",
                    LogLevel::LOG_DEBUG);
      lattice::mesh::Mesh::sendDownlinkToNodeStatic(
          destMac, static_cast<adapter_types>(msg.data_type), fwdData);
    }
  } else {
    LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_WARN, "Unknown message type: %u",
                 (unsigned)msg.message_type);
  }

  // Control opcodes (CONFIG_SET/NODE_ID_SET/HEALTH_REQ/TX_POWER_SET), sent by
  // the server directly to this (master) node over serial. Phase H2 item V:
  // shared with the mesh-received path through Adapter::dispatchControlOp
  // (see Adapter.h/.cpp) instead of being handled again here.
  // rebroadcastOnMaster=true: a TX_POWER_SET originating here (direct serial)
  // must still propagate to the rest of the mesh, unlike one arriving via the
  // mesh itself (already broadcast once).
  if (msg.data_type == adapter_types::SERIAL_ADAPTER) {
    // msg.data[0] read inline (rather than a local `op`) so that under LOG_NONE,
    // where both LATTICE_LOGF calls below fold to ((void)0), there's no
    // now-unused local left behind to warn about.
    LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_DEBUG,
                 "Processing SERIAL_ADAPTER control opcode: 0x%02X", msg.data[0]);

    bool handled = dispatchControlOp(msg, /*rebroadcastOnMaster=*/true);
    if (!handled) {
      LATTICE_LOGF("Serial_Adapter", LogLevel::LOG_WARN, "Unknown SERIAL_ADAPTER opcode: 0x%02X",
                   msg.data[0]);
    }
  }

  LATTICE_LOGLN("Serial_Adapter", "Frame processing completed successfully", LogLevel::LOG_DEBUG);
}

} // namespace adapter
} // namespace lattice
