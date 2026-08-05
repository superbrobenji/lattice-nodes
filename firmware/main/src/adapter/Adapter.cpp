#include "Adapter.h"
#include "src/mesh/Mesh.h" // for full definition of mesh_message
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <cstdio>
#include <cstring>
#include <esp_wifi.h>
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/persistence/EepromManager.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "src/network/hw_mac.h"
#include "src/network/MacEq.h"

namespace lattice {
namespace adapter {

using namespace lattice::utils;

Adapter::Adapter(uint8_t pin)
    : _pin(pin), _adapterType(adapter_types::UNKNOWN_ADAPTER), mesh_transmit_fn(nullptr) {
  LATTICE_LOGLN("Adapter", "Base adapter initialized with UNKNOWN_ADAPTER", LogLevel::LOG_DEBUG);
}

adapter_types Adapter::getAdapterType() const {
  return _adapterType;
}

void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[64]) {
  if (mesh_transmit_fn) {
    mesh_transmit_fn(type, data);
    LATTICE_LOGLN("Adapter", "Data sent through mesh", LogLevel::LOG_DEBUG);
  } else {
    lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                       1, "Adapter: Transmit function not set");
  }
}

void Adapter::setTransmitFn(TransmitPtr fn) {
  mesh_transmit_fn = fn;
  LATTICE_LOGLN("Adapter", "Transmit function assigned", LogLevel::LOG_DEBUG);
}

void Adapter::onMeshData(const lattice::mesh::mesh_message& message) {
  // Control opcodes (OP_CONFIG_SET, OP_NODE_ID_SET, OP_HEALTH_REQ,
  // OP_TX_POWER_SET) all travel as SERIAL_ADAPTER-typed data. CONFIG_SET and
  // NODE_ID_SET must run here, in the base class, for ALL node types so that
  // virtual dispatch to a per-type no-op (e.g. PIR's onMeshDataImpl) cannot
  // swallow them — see dispatchControlOp/opConfigSet/opNodeIdSet below.
  // HEALTH_REQ/TX_POWER_SET are meaningful only to a serial-attached master
  // and no-op internally for other adapter types (opHealthReq/opTxPowerSet),
  // reproducing the original behavior where they were reachable only via
  // SerialAdapter's own code path.
  if (message.data_type == adapter_types::SERIAL_ADAPTER) {
    dispatchControlOp(message, /*rebroadcastOnMaster=*/false);
    // Other SERIAL_ADAPTER message types (i.e. not a recognized control
    // opcode) are Serial-node-specific; fall through to onMeshDataImpl so
    // Serial_Adapter can handle them (e.g. forwarding to the server).
    if (_adapterType != adapter_types::SERIAL_ADAPTER)
      return;
  }

  // Normal per-adapter dispatch.
  //
  // The SerialAdapter is the master's uplink to the server: its onMeshDataImpl
  // encodes the frame and writes it out over serial. The master must forward
  // EVERY adapter-data frame it receives from the mesh (PIR motion, LED acks,
  // any node telemetry) to the server -- regardless of the frame's data_type --
  // so it must never be filtered out here. Without this, a master running the
  // SerialAdapter (data_type SERIAL_ADAPTER) silently dropped all sensor
  // telemetry whose data_type differed from its own, and node data never
  // reached the server. Every other adapter type only handles frames matching
  // its own data_type.
  if (_adapterType != adapter_types::SERIAL_ADAPTER && message.data_type != _adapterType)
    return;
  onMeshDataImpl(message);
}

void Adapter::onMeshDataImpl(const lattice::mesh::mesh_message& /*message*/) {
  // Default no-op in base; subclasses optionally override
}

// ---------------------------------------------------------------------------
// Phase H2 audit item W: health-report frame builder + shared interval tick.
// ---------------------------------------------------------------------------

void Adapter::buildHealthFrame(uint8_t opcode, uint8_t* buf, size_t bufsize) const {
  if (buf == nullptr || bufsize < 12)
    return;
  memset(buf, 0, bufsize);
  buf[0] = opcode;
  buf[1] = lattice::adapter::AdapterFactory::adapterTypeToEEPROM(_adapterType);

  uint8_t mac[6];
  lattice::hw::readOwnMac(mac);
  memcpy(&buf[2], mac, 6);

  uint32_t uptimeSec = millis() / 1000;
  buf[8] = static_cast<uint8_t>(uptimeSec & 0xFF);
  buf[9] = static_cast<uint8_t>((uptimeSec >> 8) & 0xFF);
  buf[10] = static_cast<uint8_t>((uptimeSec >> 16) & 0xFF);
  buf[11] = static_cast<uint8_t>((uptimeSec >> 24) & 0xFF);
}

void Adapter::sendSelfHealthReport() const {
  uint8_t data[64] = {0};
  buildHealthFrame(OP_HEALTH_REPORT, data, sizeof(data));
  // This report is ABOUT this node, so use transmitSelfOriginated(): on a
  // master node, plain transmit() would only broadcast it to mesh peers
  // (who don't need it) and never deliver it to the master's own serial
  // port — the only place the server can ever see it.
  lattice::mesh::Mesh::transmitSelfOriginated(adapter_types::SERIAL_ADAPTER, data);
  LATTICE_LOGLN("Adapter", "Health report sent via self-originated transmit", LogLevel::LOG_DEBUG);
}

bool Adapter::healthTickDue(uint32_t now) {
  if (now - _lastHealthMillis >= lattice::config::HEALTH_REPORT_INTERVAL_MS) {
    _lastHealthMillis = now;
    return true;
  }
  return false;
}

void Adapter::resetHealthTick(uint32_t now) {
  _lastHealthMillis = now;
}

// ---------------------------------------------------------------------------
// Phase H2 audit item V: shared control-op dispatch table.
// ---------------------------------------------------------------------------

bool Adapter::isTargetedAtSelf(const uint8_t* candidateMac) {
  uint8_t ownMac[6];
  lattice::hw::readOwnMac(ownMac);
  // Accept broadcast (FF:FF:FF:FF:FF:FF) or unicast to our MAC
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (candidateMac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return allFF || lattice::mac::eq(candidateMac, ownMac);
}

void Adapter::opConfigSet(const lattice::mesh::mesh_message& message, bool /*rebroadcastOnMaster*/) {
  // Wire format: [C1][6B targetMac][1B adapterType] (opcodes.h).
  if (!isTargetedAtSelf(&message.data[1])) {
    LATTICE_LOGLN("ADAPTER", "CONFIG_SET not targeted to this node, ignoring", LogLevel::LOG_DEBUG);
    return;
  }
  adapter_types newType = lattice::adapter::AdapterFactory::adapterTypeFromEEPROM(message.data[7]);
  lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
  LATTICE_LOGLN("ADAPTER", "CONFIG_SET received, restarting with new adapter type",
                LogLevel::LOG_INFO);
  ESP.restart();
}

void Adapter::opNodeIdSet(const lattice::mesh::mesh_message& message, bool /*rebroadcastOnMaster*/) {
  // Wire format: [C0][6B targetMAC][1B nodeId] (opcodes.h).
  if (!isTargetedAtSelf(&message.data[1]))
    return;
  uint8_t nodeId = message.data[7];
  lattice::eeprom::saveNodeId(nodeId);
  char buf[32];
  snprintf(buf, sizeof(buf), "Node ID set: %u", (unsigned)nodeId);
  LATTICE_LOGLN("ADAPTER", buf, LogLevel::LOG_INFO);
}

void Adapter::opHealthReq(const lattice::mesh::mesh_message& /*message*/,
                          bool /*rebroadcastOnMaster*/) {
  // Only the serial-attached master answers HEALTH_REQ over its own serial link.
  if (_adapterType != adapter_types::SERIAL_ADAPTER)
    return;
  LATTICE_LOGLN("ADAPTER", "Received health request, sending health report", LogLevel::LOG_INFO);
  sendSelfHealthReport();
}

void Adapter::opTxPowerSet(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster) {
  // TX power is a radio-wide setting owned by the serial-attached master.
  if (_adapterType != adapter_types::SERIAL_ADAPTER)
    return;

  uint8_t presetByte = message.data[1];
  if (presetByte > 2) {
    LATTICE_LOGLN("ADAPTER", "Invalid TX power preset, ignoring", LogLevel::LOG_WARN);
    return;
  }

  auto preset = static_cast<lattice::config::TxPowerPreset>(presetByte);
  lattice::eeprom::saveTxPowerPreset(preset);
  esp_err_t txErr = esp_wifi_set_max_tx_power(
      static_cast<int8_t>(lattice::config::TX_POWER_VALUES[presetByte]));
  if (txErr != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "TX power set failed: %s", esp_err_to_name(txErr));
    LATTICE_LOGLN("ADAPTER", buf, LogLevel::LOG_WARN);
  } else {
    LATTICE_LOGLN("ADAPTER", "TX power preset applied", LogLevel::LOG_INFO);
  }

  // Only propagate onward when the change originated locally (direct-serial
  // entry point) — a mesh-received TX_POWER_SET has already been broadcast
  // once and must not be re-broadcast (would loop).
  if (!rebroadcastOnMaster)
    return;
  lattice::mesh::Mesh* meshPtr = lattice::mesh::Mesh::getInstance();
  if (meshPtr && meshPtr->getIsMaster()) {
    uint8_t fwdData[64] = {};
    fwdData[0] = OP_TX_POWER_SET;
    fwdData[1] = presetByte;
    lattice::mesh::Mesh::broadcastAdapterDataStatic(adapter_types::SERIAL_ADAPTER, fwdData);
    LATTICE_LOGLN("ADAPTER", "TX power preset broadcast to mesh", LogLevel::LOG_INFO);
  }
}

const Adapter::ControlOpEntry Adapter::kControlOps[] = {
    {OP_CONFIG_SET, &Adapter::opConfigSet},
    {OP_NODE_ID_SET, &Adapter::opNodeIdSet},
    {OP_HEALTH_REQ, &Adapter::opHealthReq},
    {OP_TX_POWER_SET, &Adapter::opTxPowerSet},
};
const size_t Adapter::kControlOpCount = sizeof(kControlOps) / sizeof(kControlOps[0]);

bool Adapter::dispatchControlOp(const lattice::mesh::mesh_message& message,
                                bool rebroadcastOnMaster) {
  const uint8_t op = message.data[0];
  for (size_t i = 0; i < kControlOpCount; ++i) {
    if (kControlOps[i].opcode == op) {
      (this->*kControlOps[i].handler)(message, rebroadcastOnMaster);
      return true;
    }
  }
  return false;
}

bool Adapter::init() {
  return true;
}

} // namespace adapter
} // namespace lattice
