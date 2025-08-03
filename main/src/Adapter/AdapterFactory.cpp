#include "AdapterFactory.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

Adapter* AdapterFactory::createAdapter(adapter_types type, int pin) {
  if (!newAdapter) {
    ErrorHandler::getInstance().signalError(ErrorType::MEMORY_ERROR, "AdapterFactory: Out of memory creating adapter");
    return nullptr;
  }
  switch (type) {
    case PIR_ADAPTER:
      Logger::logln("Factory", "Creating PIR_Adapter");
      return new PIR_Adapter(pin);

      // case WIFI_ADAPTER:
      //   Logger::logln("Factory", "Creating WiFi_Adapter");
      //   return new WiFi_Adapter(pin);

      // case LED_ADAPTER:
      //   Logger::logln("Factory", "Creating LED_Adapter");
      //   return new LED_Adapter(pin);

    default:
      ErrorHandler::getInstance().signalError(
        ErrorType::CONFIG_ERROR,
        "AdapterFactory: Unknown adapter type");
      return nullptr;
  }
}

}
}
