#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "src/core/Logger.h"
#include "src/core/ErrorHandler.h"

namespace planetopia {
namespace utils {

// Network status
enum class NetworkStatus {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  ERROR
};

// MAC address utilities
struct MacAddress {
  uint8_t bytes[6];

  // Constructors
  MacAddress();
  MacAddress(const uint8_t* mac);
  MacAddress(const String& macString);

  // Comparison operators
  bool operator==(const MacAddress& other) const;
  bool operator!=(const MacAddress& other) const;

  // Utility methods
  String toString() const;
  bool isValid() const;
  bool isBroadcast() const;
  bool isZero() const;
  void setBroadcast();
  void setZero();

  // Static methods
  static MacAddress getOwnMac();
  static MacAddress fromString(const String& macString);
  static bool isValidMacString(const String& macString);
};

class NetworkManager {
public:
  // Constructor
  NetworkManager();

  // Core network operations
  bool init();
  void shutdown();
  NetworkStatus getStatus() const;

  // MAC address management
  MacAddress getOwnMac() const;
  bool setOwnMac(const MacAddress& mac);
  bool isMacValid(const MacAddress& mac) const;

  // WiFi operations
  bool startWiFi();
  bool stopWiFi();
  bool isWiFiActive() const;
  int getWiFiChannel() const;
  int getRSSI() const;

  // Network scanning
  bool scanNetworks();
  int getScannedNetworkCount() const;
  String getScannedNetworkSSID(int index) const;
  int getScannedNetworkRSSI(int index) const;
  MacAddress getScannedNetworkMAC(int index) const;

  // Configuration
  void setWiFiMode(wifi_mode_t mode);
  void setWiFiChannel(int channel);
  void setWiFiPower(int power);

  // Event handling
  void onWiFiEvent(WiFiEvent_t event);
  void registerEventCallback(std::function<void(WiFiEvent_t)> callback);
  void unregisterEventCallback();

private:
  NetworkStatus status_;
  MacAddress ownMac_;
  bool wifiActive_;
  wifi_mode_t wifiMode_;
  int wifiChannel_;
  int wifiPower_;

  std::function<void(WiFiEvent_t)> eventCallback_;

  // Internal helper methods
  bool initializeWiFi();
  void logNetworkEvent(const char* event, const char* details = nullptr);
  void handleWiFiError(esp_err_t error, const char* operation);
  bool validateWiFiConfig() const;
};

}  // namespace utils
}  // namespace planetopia

#endif  // NETWORK_MANAGER_H
