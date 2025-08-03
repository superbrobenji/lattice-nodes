#include "AdapterFactory.h":LOG_INFO)
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

Adapter* AdapterFactory::createAdapter(adapter_types type, int pin) {
  switch (type) {
    case PIR_ADAPTER:
      Logger::logln("Factory", "Creating PIR_Adapter", LogLevel::LOG_INFO);
      return new PIR_Adapter(pin);

      // case WIFI_ADAPTER:
      //   Logger::logln("Factory", "Creating WiFi_Adapter", LogLevel::LOG_INFO);
      //   return new WiFi_Adapter(pin);

      // case LED_ADAPTER:
      //   Logger::logln("Factory", "Creating LED_Adapter", LogLevel::LOG_INFO);
      //   return new LED_Adapter(pin);

    default:
      ErrorHandler::getInstance().signalError(
        ErrorType::CONFIG_ERROR,
        "AdapterFactory: Unknown adapter type"
      );
      Logger::logln("Factory", "Error: Unknown adapter type", LogLevel::LOG_ERROR);
      return nullptr;
  }
}

}
}
