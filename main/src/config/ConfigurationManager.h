#ifndef CONFIGURATION_MANAGER_H
#define CONFIGURATION_MANAGER_H

#include <Arduino.h>
#include "src/persistence/EEPROM_Manager.h"
#include "src/core/Logger.h"
#include "src/core/ErrorHandler.h"

namespace planetopia {
namespace utils {

// Configuration types
enum class ConfigType {
  MASTER_FLAG,
  DEV_MODE,
  ADAPTER_TYPE,
  MESH_KEY,
  PEER_LIST
};

// Configuration validation result
struct ConfigValidationResult {
  bool isValid;
  String errorMessage;
  String suggestion;
};

class ConfigurationManager {
public:
  // Constructor
  ConfigurationManager();

  // Core configuration methods
  bool setConfiguration(ConfigType type, const void* data, size_t dataSize);
  bool getConfiguration(ConfigType type, void* data, size_t dataSize);
  bool hasConfiguration(ConfigType type) const;

  // Configuration validation
  ConfigValidationResult validateConfiguration(ConfigType type, const void* data, size_t dataSize);
  bool isConfigurationValid(ConfigType type, const void* data, size_t dataSize);

  // Development mode management
  void setDevMode(bool isDev);
  bool isDevMode() const;
  void toggleDevMode();

  // Configuration persistence
  bool saveConfiguration(ConfigType type);
  bool loadConfiguration(ConfigType type);
  void clearConfiguration(ConfigType type);
  void clearAllConfigurations();

  // Configuration status
  bool isConfigurationDirty(ConfigType type) const;
  bool isAnyConfigurationDirty() const;
  void markConfigurationClean(ConfigType type);

  // Configuration change callbacks
  using ConfigChangeCallback = std::function<void(ConfigType, const void*, size_t)>;
  void registerConfigChangeCallback(ConfigChangeCallback callback);
  void unregisterConfigChangeCallback();

  // Utility methods
  String getConfigurationString(ConfigType type) const;
  void printConfigurationStatus() const;
  void resetToDefaults();

private:
  // Configuration state
  struct ConfigState {
    bool isSet;
    bool isDirty;
    uint8_t data[64];  // Max config size
    size_t dataSize;
    unsigned long lastModified;
  };

  std::map<ConfigType, ConfigState> configStates_;
  bool devMode_;
  ConfigChangeCallback configChangeCallback_;

  // Internal helper methods
  bool validateMasterFlag(const void* data, size_t dataSize) const;
  bool validateDevMode(const void* data, size_t dataSize) const;
  bool validateAdapterType(const void* data, size_t dataSize) const;
  bool validateMeshKey(const void* data, size_t dataSize) const;
  bool validatePeerList(const void* data, size_t dataSize) const;

  void notifyConfigChange(ConfigType type, const void* data, size_t dataSize);
  void logConfigOperation(const char* operation, ConfigType type, bool success);
  String configTypeToString(ConfigType type) const;
};

}  // namespace utils
}  // namespace planetopia

#endif  // CONFIGURATION_MANAGER_H
