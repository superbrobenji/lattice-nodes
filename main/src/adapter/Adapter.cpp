#include "Adapter.h"
#include "src/mesh/Mesh.h" // for full definition of mesh_message
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/persistence/EepromManager.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include <esp_wifi.h>
#include <cstring>

namespace lattice {
namespace adapter {

using namespace lattice::utils;

Adapter::Adapter(int pin)
    : _pin(pin), _adapterType(adapter_types::UNKNOWN_ADAPTER), mesh_transmit_fn(nullptr) {
  Logger::logln("Adapter", "Base adapter initialized with UNKNOWN_ADAPTER", LogLevel::LOG_DEBUG);
}

adapter_types Adapter::getAdapterType() const {
  return _adapterType;
}

void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[64]) {
  if (mesh_transmit_fn) {
    mesh_transmit_fn(type, data);
    Logger::logln("Adapter", "Data sent through mesh", LogLevel::LOG_DEBUG);
  } else {
    lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::ADAPTER,
                       1, "Adapter: Transmit function not set");
  }
}

void Adapter::setTransmitFn(TransmitPtr fn) {
  mesh_transmit_fn = fn;
  Logger::logln("Adapter", "Transmit function assigned", LogLevel::LOG_DEBUG);
}

void Adapter::onMeshData(const lattice::mesh::mesh_message& message) {
  // Handle OP_CONFIG_SET for ALL adapter types — server can reconfigure any node.
  // This must run in the base class so virtual dispatch to per-type no-ops cannot
  // swallow the opcode on PIR/LED/WiFi nodes.
  // OP_CONFIG_SET = 0xC1 (from lib/lattice-protocol/opcodes.h)
  if (message.data_type == adapter_types::SERIAL_ADAPTER) {
    const uint8_t op = message.data[0];
    if (op == OP_CONFIG_SET) {
      uint8_t ownMac[6];
      esp_wifi_get_mac(WIFI_IF_STA, ownMac);
      // Accept broadcast (FF:FF:FF:FF:FF:FF) or unicast to our MAC
      bool allFF = true;
      for (int i = 0; i < 6; ++i) {
        if (message.data[1 + i] != 0xFF) {
          allFF = false;
          break;
        }
      }
      bool isTarget = allFF || (memcmp(&message.data[1], ownMac, 6) == 0);
      if (isTarget) {
        adapter_types newType =
            lattice::adapter::AdapterFactory::adapterTypeFromEEPROM(message.data[7]);
        lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(newType);
        Logger::logln("ADAPTER", "CONFIG_SET received, restarting with new adapter type",
                      LogLevel::LOG_INFO);
        ESP.restart();
      } else {
        Logger::logln("ADAPTER", "CONFIG_SET not targeted to this node, ignoring",
                      LogLevel::LOG_DEBUG);
      }
    }
    if (op == OP_NODE_ID_SET) {
      uint8_t ownMac[6];
      esp_wifi_get_mac(WIFI_IF_STA, ownMac);
      bool allFF = true;
      for (int i = 0; i < 6; ++i) {
        if (message.data[1 + i] != 0xFF) {
          allFF = false;
          break;
        }
      }
      bool isTarget = allFF || (memcmp(&message.data[1], ownMac, 6) == 0);
      if (isTarget) {
        uint8_t nodeId = message.data[7];
        lattice::utils::EepromManager::getInstance().saveNodeId(nodeId);
        Logger::logln("ADAPTER", "Node ID assigned: " + String(nodeId), LogLevel::LOG_INFO);
      }
    }
    // Other SERIAL_ADAPTER opcodes (e.g. OP_HEALTH_REQ) are Serial-node-specific;
    // fall through to onMeshDataImpl so Serial_Adapter can handle them.
    if (_adapterType != adapter_types::SERIAL_ADAPTER)
      return;
  }

  // Handle LED output commands for LED_ADAPTER nodes.
  // No RGB LED driver is currently implemented in firmware — these are logged stubs.
  // TODO: When an LED adapter (e.g. NeoPixel/WS2812) is wired up, replace the Logger
  // calls below with the appropriate driver calls (e.g. ledStrip.setPixelColor(r, g, b)).
  if (message.data_type == adapter_types::LED_ADAPTER &&
      _adapterType == adapter_types::LED_ADAPTER) {
    const uint8_t op = message.data[0];
    if (op == OP_LED_SOLID) {
      uint8_t r = message.data[1];
      uint8_t g = message.data[2];
      uint8_t b = message.data[3];
      Logger::logln("ADAPTER",
                    String("OP_LED_SOLID: R=") + String(r) + " G=" + String(g) + " B=" + String(b) +
                        " (stub — no RGB driver)",
                    LogLevel::LOG_INFO);
      // Send OP_COMMAND_ACK back through the mesh
      uint8_t ackData[12] = {0};
      ackData[0] = OP_COMMAND_ACK;
      ackData[1] = op; // echo the handled opcode as the command ID
      sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, ackData);
    } else if (op == OP_LED_OFF) {
      Logger::logln("ADAPTER", "OP_LED_OFF (stub — no RGB driver)", LogLevel::LOG_INFO);
      uint8_t ackData[12] = {0};
      ackData[0] = OP_COMMAND_ACK;
      ackData[1] = op;
      sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, ackData);
    } else if (op == OP_LED_BLINK) {
      Logger::logln("ADAPTER", "OP_LED_BLINK (stub — no RGB driver)", LogLevel::LOG_INFO);
      uint8_t ackData[12] = {0};
      ackData[0] = OP_COMMAND_ACK;
      ackData[1] = op;
      sendDataThroughMesh(adapter_types::SERIAL_ADAPTER, ackData);
    } else if (op == OP_RELAY_SET) {
      Logger::logln("ADAPTER", "OP_RELAY_SET received on LED_ADAPTER (unexpected)",
                    LogLevel::LOG_WARN);
    }
    return;
  }

  // Normal per-adapter dispatch
  if (message.data_type != _adapterType)
    return;
  onMeshDataImpl(message);
}

void Adapter::onMeshDataImpl(const lattice::mesh::mesh_message& /*message*/) {
  // Default no-op in base; subclasses optionally override
}

bool Adapter::init() {
  return true;
}

} // namespace adapter
} // namespace lattice
