#include "Adapter.h"
#include "src/Mesh/Mesh.h"  // for full definition of mesh_message
#include "src/core/Logger.h"
#include "src/error/Error.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

Adapter::Adapter(int pin)
  : _pin(pin), _adapterType(UNKNOWN_ADAPTER), mesh_transmit_fn(nullptr) {
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
