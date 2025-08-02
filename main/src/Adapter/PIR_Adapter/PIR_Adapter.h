#ifndef PIR_ADAPTER_H
#define PIR_ADAPTER_H

#include <Arduino.h>
#include "src/Adapter/Adapter.h"

namespace planetopia {
namespace adapter {

class PIR_Adapter : public Adapter {
private:
  int _pin;                    // GPIO pin for PIR sensor
  int _cooldownSeconds;        // Delay before allowing new trigger
  unsigned long _lastTrigger;  // Timestamp of last motion detection

  bool _timerActive;              // Whether cooldown timer is running
  bool _motionSent;               // Whether motion message has been sent
  bool _interruptEnabled;         // Whether interrupt is currently active
  bool _initialized;              // Prevent re-initialization
  volatile bool _motionDetected;  // Flag set by ISR (safe from IRAM_ATTR)

  static PIR_Adapter* instance;  // Singleton pointer for ISR redirection

  static void detectMotionTrampoline();                                         // Static trampoline for ISR
  static void sendDataTrampoline(adapter_types adapterType, uint8_t data[12]);  // Static trampoline for Mesh messages

  void IRAM_ATTR detectMotion();  // Actual ISR logic (minimal)

public:
  explicit PIR_Adapter(int pin);  // Constructor with pin assignment
  void init() override;           // Sets up the PIR adapter
  void loop() override;           // Called repeatedly in Arduino loop
};

}
}

#endif
