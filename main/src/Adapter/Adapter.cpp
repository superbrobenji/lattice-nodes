#include "Adapter.h"
#include "src/Mesh/Mesh.h"  // for full definition of mesh_message
#include "src/core/Logger.h"
#include "src/error/Error.h"
#include "src/Adapter/AdapterFactory.h"
#include <esp_wifi.h>
#include <cstring>

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

Adapter::Adapter(int pin)
  : _pin(pin), _adapterType(adapter_types::UNKNOWN_ADAPTER), mesh_transmit_fn(nullptr) {
  Logger::logln("Adapter", "Base adapter initialized with UNKNOWN_ADAPTER", LogLevel::LOG_DEBUG);
}

adapter_types Adapter::getAdapterType() const {
  return _adapterType;
}

void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[12]) {
  if (mesh_transmit_fn) {
    mesh_transmit_fn(type, data);
    Logger::logln("Adapter", "Data sent through mesh", LogLevel::LOG_DEBUG);
  } else {
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                         planetopia::core::ModuleDigit::ADAPTER,
                         1,
                         "Adapter: Transmit function not set");
  }
}

void Adapter::setTransmitFn(TransmitPtr fn) {
  mesh_transmit_fn = fn;
  Logger::logln("Adapter", "Transmit function assigned", LogLevel::LOG_DEBUG);
}

void Adapter::onMeshData(const planetopia::mesh::mesh_message& message) {
  // Handle OP_CONFIG_SET for ALL adapter types — server can reconfigure any node.
  // This must run in the base class so virtual dispatch to per-type no-ops cannot
  // swallow the opcode on PIR/LED/WiFi nodes.
  static constexpr uint8_t OP_CONFIG_SET = 0xA0;
  if (message.dataType == adapter_types::SERIAL_ADAPTER) {
    const uint8_t op = message.data[0];
    if (op == OP_CONFIG_SET) {
      uint8_t ownMac[6];
      esp_wifi_get_mac(WIFI_IF_STA, ownMac);
      // Accept broadcast (FF:FF:FF:FF:FF:FF) or unicast to our MAC
      bool allFF = true;
      for (int i = 0; i < 6; ++i) {
        if (message.data[1 + i] != 0xFF) { allFF = false; break; }
      }
      bool isTarget = allFF || (memcmp(&message.data[1], ownMac, 6) == 0);
      if (isTarget) {
        adapter_types newType = planetopia::adapter::AdapterFactory::adapterTypeFromEEPROM(message.data[7]);
        planetopia::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        Logger::logln("ADAPTER", "CONFIG_SET received, restarting with new adapter type", LogLevel::LOG_INFO);
        ESP.restart();
      } else {
        Logger::logln("ADAPTER", "CONFIG_SET not targeted to this node, ignoring", LogLevel::LOG_DEBUG);
      }
    }
    // Other SERIAL_ADAPTER opcodes (e.g. OP_HEALTH_REQ) are Serial-node-specific;
    // fall through to onMeshDataImpl so Serial_Adapter can handle them.
    if (_adapterType != adapter_types::SERIAL_ADAPTER) return;
  }

  // Normal per-adapter dispatch
  if (message.dataType != _adapterType) return;
  onMeshDataImpl(message);
}

void Adapter::onMeshDataImpl(const planetopia::mesh::mesh_message& /*message*/) {
  // Default no-op in base; subclasses optionally override
}


bool Adapter::init() {
  return true;
}

}
}
