#include "AdapterFactory.h"
#include "src/core/Logger.h"
#include "src/core/ErrorHandler.h"
#include "src/persistence/EEPROM_Manager.h"
// Include all adapter headers
#include "src/Adapter/PIR_Adapter/PIR_Adapter.h"
#include "src/Adapter/Serial_Adapter/Serial_Adapter.h"

namespace planetopia {
namespace adapter {

using namespace planetopia::utils;

// Static dev mode flag
static bool isDevMode = false;

void AdapterFactory::setDevMode(bool isDev) {
  isDevMode = isDev;
  Logger::logln("Factory", String("Dev mode ") + (isDev ? "enabled" : "disabled"), LogLevel::LOG_INFO);
}

Adapter* AdapterFactory::createAdapter(adapter_types type, int pin) {
  switch (type) {
    case PIR_ADAPTER:
      Logger::logln("Factory", "Creating PIR_Adapter", LogLevel::LOG_INFO);
      return new PIR_Adapter(pin);

    case SERIAL_ADAPTER:
      Logger::logln("Factory", "Creating Serial_Adapter", LogLevel::LOG_INFO);
      return new Serial_Adapter(pin);

      // case WIFI_ADAPTER:
      //   Logger::logln("Factory", "Creating WiFi_Adapter", LogLevel::LOG_INFO);
      //   return new WiFi_Adapter(pin);

      // case LED_ADAPTER:
      //   Logger::logln("Factory", "Creating LED_Adapter", LogLevel::LOG_INFO);
      //   return new LED_Adapter(pin);

    default:
      ErrorHandler::getInstance().signalError(
        ErrorType::CONFIG_ERROR,
        "AdapterFactory: Unknown adapter type");
      Logger::logln("Factory", "Error: Unknown adapter type", LogLevel::LOG_ERROR);
      return nullptr;
  }
}

adapter_types AdapterFactory::loadAdapterTypeFromEEPROM() {
  if (isDevMode) {
    Logger::logln("Factory", "Dev mode: returning default PIR adapter type", LogLevel::LOG_DEBUG);
    return PIR_ADAPTER;  // Always return default in dev mode
  }

  uint8_t adapterType = EEPROM_Manager::getInstance().loadAdapterType();
  return static_cast<adapter_types>(static_cast<int8_t>(adapterType));
}

void AdapterFactory::saveAdapterTypeToEEPROM(adapter_types type) {
  if (isDevMode) {
    Logger::logln("Factory", "Dev mode: skipping EEPROM save for adapter type", LogLevel::LOG_DEBUG);
    return;  // Don't save to EEPROM in dev mode
  }

  EEPROM_Manager::getInstance().saveAdapterType(static_cast<uint8_t>(static_cast<int8_t>(type)));
}

Adapter* AdapterFactory::createFromEEPROM() {
  adapter_types type = loadAdapterTypeFromEEPROM();
  int pin = getDefaultPinForAdapter(type);
  return createAdapter(type, pin);
}

void AdapterFactory::initializeDefaultsIfUnset() {
  if (isDevMode) {
    Logger::logln("Factory", "Dev mode: skipping EEPROM initialization", LogLevel::LOG_DEBUG);
    return;  // Don't initialize EEPROM in dev mode
  }

  // Check if adapter type is unset (0xFF) and set default if needed
  uint8_t currentType = EEPROM_Manager::getInstance().loadAdapterType();
  if (currentType == 0xFF) {
    EEPROM_Manager::getInstance().saveAdapterType(static_cast<uint8_t>(static_cast<int8_t>(PIR_ADAPTER)));
  }
}

int AdapterFactory::getDefaultPinForAdapter(adapter_types type) {
  switch (type) {
    case PIR_ADAPTER:
      return PIR_ADAPTER_DEFAULT_PIN;
    case WIFI_ADAPTER:
      return WIFI_ADAPTER_DEFAULT_PIN;
    case LED_ADAPTER:
      return LED_ADAPTER_DEFAULT_PIN;
    case SERIAL_ADAPTER:
      return SERIAL_ADAPTER_DEFAULT_PIN;
    default:
      return PIR_ADAPTER_DEFAULT_PIN;  // fallback
  }
}

}
}
