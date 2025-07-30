#ifndef ADAPTER_H
#define ADAPTER_H

#include <Arduino.h>

// Enum for identifying adapter types
enum adapter_types {
  UNKNOWN_ADAPTER = -1,
  PIR_ADAPTER,
  WIFI_ADAPTER,
  LED_ADAPTER
};

// Abstract base class for all adapters
class Adapter {

protected:
  int _pin;                    // Hardware pin associated with the adapter
  adapter_types _adapterType;  // Type identifier for the adapter
  typedef void (*TransmitPtr)(adapter_types, const uint8_t[12]);
  TransmitPtr mesh_transmit_fn;

public:
  explicit Adapter(int pin);     // Constructor
  virtual ~Adapter() = default;  // Ensure polymorphic destruction

  adapter_types getAdapterType() const;                                        // Returns the adapter type
  void sendDataThroughMesh(const adapter_types type, const uint8_t data[12]);  // sends data through mesh
  void setTransmitFn(TransmitPtr fn);

  virtual void init() = 0;                             // To be implemented by derived classes
  virtual void loop() = 0;                             // Called repeatedly in the main loop
  virtual void recvDataFromAdapter(uint8_t data[12]);  // Optional override
};

#endif
