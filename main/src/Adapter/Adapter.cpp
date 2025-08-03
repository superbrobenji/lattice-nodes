#include "Adapter.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

Adapter::Adapter(int pin)
  : _pin(pin), _adapterType(UNKNOWN_ADAPTER), mesh_transmit_fn(nullptr) {
  Logger::logln("Adapter", "Base adapter initialized with UNKNOWN_ADAPTER");
}

adapter_types Adapter::getAdapterType() const {
  return _adapterType;
}

void Adapter::sendDataThroughMesh(const adapter_types type, const uint8_t data[12]) {
  if (mesh_transmit_fn) {
    mesh_transmit_fn(type, data);
    Logger::logln("Adapter", "Data sent through mesh");
  } else {
    ErrorHandler::getInstance().signalError(
      ErrorType::CONFIG_ERROR,
      "Adapter: Transmit function not set");
  }
}

void Adapter::setTransmitFn(TransmitPtr fn) {
  mesh_transmit_fn = fn;
  Logger::logln("Adapter", "Transmit function assigned");
}

void Adapter::recvDataFromAdapter(uint8_t data[12]) {
  // Optional override – default does nothing
  Logger::logln("Adapter", "recvDataFromAdapter called (default, no-op)");
}

bool Adapter::init() {
  // Base class default (no-op)
  return true;
}

}
}
