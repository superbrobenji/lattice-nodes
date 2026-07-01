# Adapter Development Guide

This guide explains how to add new adapters to the Lattice system and how to change the default adapter for testing purposes.

## Table of Contents

1. [Adding a New Adapter](#adding-a-new-adapter)
2. [Changing the Default Adapter](#changing-the-default-adapter)
3. [Adapter Architecture Overview](#adapter-architecture-overview)
4. [Testing Your New Adapter](#testing-your-new-adapter)

## Adding a New Adapter

### Step 1: Create the Adapter Header File

Create a new header file in `main/src/Adapter/[AdapterName]_Adapter/[AdapterName]_Adapter.h`:

```cpp
#ifndef [ADAPTERNAME]_ADAPTER_H
#define [ADAPTERNAME]_ADAPTER_H

#include "src/Adapter/Adapter.h"

namespace lattice {
namespace adapter {

class [AdapterName]_Adapter : public Adapter {
public:
    [AdapterName]_Adapter(int pin);
    ~[AdapterName]_Adapter() = default;

    // Required overrides
    void loop() override;
    void onMeshDataImpl(const lattice::mesh::mesh_message& message) override;

private:
    int _pin;
    // Add your adapter-specific member variables here
    
    // Add your adapter-specific helper methods here
};

} // namespace adapter
} // namespace lattice

#endif // [ADAPTERNAME]_ADAPTER_H
```

**Example for a Temperature Sensor Adapter:**
```cpp
#ifndef TEMP_ADAPTER_H
#define TEMP_ADAPTER_H

#include "src/Adapter/Adapter.h"

namespace lattice {
namespace adapter {

class Temp_Adapter : public Adapter {
public:
    Temp_Adapter(int pin);
    ~Temp_Adapter() = default;

    void loop() override;
    void onMeshDataImpl(const lattice::mesh::mesh_message& message) override;

private:
    int _pin;
    float _lastTemperature;
    unsigned long _lastReadTime;
    
    void readTemperature();
    void sendTemperatureData();
};

} // namespace adapter
} // namespace lattice

#endif // TEMP_ADAPTER_H
```

### Step 2: Create the Adapter Implementation File

Create the corresponding `.cpp` file in `main/src/Adapter/[AdapterName]_Adapter/[AdapterName]_Adapter.cpp`:

```cpp
#include "[AdapterName]_Adapter.h"
#include "src/Mesh/Mesh.h"
#include "src/core/Logger.h"

namespace lattice {
namespace adapter {

[AdapterName]_Adapter::[AdapterName]_Adapter(int pin) 
    : Adapter(TEMP_ADAPTER, pin), _pin(pin) {
    // Initialize your hardware here
    // Example: pinMode(_pin, INPUT);
}

void [AdapterName]_Adapter::loop() {
    // Implement your main loop logic here
    // This runs every iteration of the main loop
    
    // Example: Read sensor every 5 seconds
    static unsigned long lastRead = 0;
    if (millis() - lastRead > 5000) {
        // Your sensor reading logic here
        lastRead = millis();
    }
}

void [AdapterName]_Adapter::onMeshDataImpl(const lattice::mesh::mesh_message& message) {
    // Handle incoming mesh messages specific to this adapter type
    // This method is only called for messages where dataType matches this adapter's type
    
    // Example: Process incoming temperature requests
    if (message.dataType == TEMP_ADAPTER) {
        // Handle temperature-related mesh messages
        // Example: Send temperature data when requested
        if (message.data[0] == 0x01) { // REQUEST_TEMP opcode
            sendTemperatureData();
        }
    }
}

// Add your private helper methods here
// Example:
// void [AdapterName]_Adapter::readTemperature() {
//     // Implementation
// }

} // namespace adapter
} // namespace lattice
```

**Example Implementation:**
```cpp
#include "Temp_Adapter.h"
#include "src/Mesh/Mesh.h"
#include "src/core/Logger.h"

namespace lattice {
namespace adapter {

Temp_Adapter::Temp_Adapter(int pin) 
    : Adapter(TEMP_ADAPTER, pin), _pin(pin), _lastTemperature(0.0), _lastReadTime(0) {
    pinMode(_pin, INPUT);
}

void Temp_Adapter::loop() {
    // Read temperature every 5 seconds
    if (millis() - _lastReadTime > 5000) {
        readTemperature();
        _lastReadTime = millis();
    }
}

void Temp_Adapter::onMeshDataImpl(const lattice::mesh::mesh_message& message) {
    if (message.dataType == TEMP_ADAPTER) {
        if (message.data[0] == 0x01) { // REQUEST_TEMP
            sendTemperatureData();
        }
    }
}

void Temp_Adapter::readTemperature() {
    // Read analog value and convert to temperature
    int rawValue = analogRead(_pin);
    _lastTemperature = (rawValue * 3.3 / 4095.0 - 0.5) * 100; // Example conversion
}

void Temp_Adapter::sendTemperatureData() {
    if (mesh_transmit_fn) {
        lattice::mesh::mesh_message msg;
        msg.messageType = lattice::mesh::MESH_TYPE_ADAPTER_DATA;
        msg.dataType = TEMP_ADAPTER;
        msg.data[0] = 0x02; // TEMP_DATA opcode
        msg.data[1] = (uint8_t)(_lastTemperature * 10); // Send temperature * 10 as integer
        
        // Set target to broadcast
        memset(msg.targetMacAddress, 0xFF, 6);
        
        mesh_transmit_fn(msg);
    }
}

} // namespace adapter
} // namespace lattice
```

### Step 3: Add the Adapter Type to the Enum

In `main/src/Adapter/Adapter.h`, add your new adapter type to the `adapter_types` enum:

```cpp
enum adapter_types {
    PIR_ADAPTER = 0,
    WIFI_ADAPTER = 1,
    LED_ADAPTER = 2,
    SERIAL_ADAPTER = 3,
    TEMP_ADAPTER = 4,  // Add your new adapter here
    // ... add more as needed
};
```

### Step 4: Add Default Pin Configuration

In `main/src/Adapter/AdapterFactory.h`, add the default pin for your adapter:

```cpp
// Default pins for each adapter type
static constexpr int PIR_ADAPTER_DEFAULT_PIN = 27;
static constexpr int WIFI_ADAPTER_DEFAULT_PIN = -1;
static constexpr int LED_ADAPTER_DEFAULT_PIN = 2;
static constexpr int SERIAL_ADAPTER_DEFAULT_PIN = -1;
static constexpr int TEMP_ADAPTER_DEFAULT_PIN = 34;  // Add your default pin here
```

### Step 5: Update the Factory

In `main/src/Adapter/AdapterFactory.cpp`, add your adapter to the `createAdapter` method:

```cpp
Adapter* AdapterFactory::createAdapter(adapter_types type, int pin) {
    switch (type) {
        case PIR_ADAPTER:
            return new PIR_Adapter(pin);
        case WIFI_ADAPTER:
            return new WIFI_Adapter(pin);
        case LED_ADAPTER:
            return new LED_Adapter(pin);
        case SERIAL_ADAPTER:
            return new Serial_Adapter(pin);
        case TEMP_ADAPTER:  // Add your case here
            return new Temp_Adapter(pin);
        default:
            Logger::logln(LogLevel::LOG_ERROR, "Unknown adapter type: %d", type);
            return nullptr;
    }
}
```

### Step 6: Include Your Adapter

In `main/src/Adapter/AdapterFactory.cpp`, add the include for your adapter:

```cpp
#include "src/Adapter/PIR_Adapter/PIR_Adapter.h"
#include "src/Adapter/Serial_Adapter/Serial_Adapter.h"
#include "src/Adapter/Temp_Adapter/Temp_Adapter.h"  // Add your include here
// ... other includes
```

## Changing the Default Adapter

### Method 1: Change the Default in Code

To change the default adapter for testing, modify `main/src/Adapter/AdapterFactory.cpp`:

```cpp
// Do not call this method directly — change DEFAULT_ADAPTER in project_config.h instead.
// AdapterFactory::initializeDefaultsIfUnset() reads from EEPROM_Manager, which already
// seeds from project_config.h::DEFAULT_ADAPTER on a blank device.
```

### Method 2: Use the Serial Configuration Command

You can also change the adapter type at runtime using the serial configuration command. Connect to the master node via serial and send:

```
// To change to TEMP_ADAPTER (type 4)
// This will be sent as a protobuf message with OP_CONFIG_SET (0xA0)
```

### Method 3: Clear EEPROM and Restart

Use the **reset button** (hold 5 seconds, then confirm within 3 seconds) — this calls
`EEPROM_Manager::getInstance().clearAll()` internally and handles the commit.

Do not call `EEPROM.write()` or `EEPROM.commit()` directly — route all EEPROM I/O
through `EEPROM_Manager` to respect DEV_MODE and address constants.

## Adapter Architecture Overview

### Base Class Structure

All adapters inherit from the `Adapter` base class:

```cpp
class Adapter {
public:
    Adapter(adapter_types type, int pin);
    virtual ~Adapter() = default;
    
    // Main interface methods
    virtual void loop() = 0;
    virtual void onMeshDataImpl(const lattice::mesh::mesh_message& message) = 0;
    
    // Mesh data handling (automatically filters by adapter type)
    void onMeshData(const lattice::mesh::mesh_message& message);
    
    // Getter methods
    adapter_types getType() const { return _adapterType; }
    int getPin() const { return _pin; }
    
    // Set transmit function
    void setTransmitFn(transmit_fn_t fn) { mesh_transmit_fn = fn; }

protected:
    adapter_types _adapterType;
    int _pin;
    transmit_fn_t mesh_transmit_fn = nullptr;
};
```

### Key Design Principles

1. **Encapsulation**: Each adapter handles its own hardware and logic
2. **Automatic Filtering**: Incoming mesh messages are automatically filtered by adapter type
3. **Standard Interface**: All adapters implement the same interface methods
4. **Pin Management**: Pins are automatically inferred based on adapter type
5. **EEPROM Persistence**: Adapter configuration is stored in EEPROM

### Message Flow

1. **Incoming Mesh Messages**: 
   - `main.ino` receives mesh messages
   - Calls `adapter->onMeshData(message)`
   - Base class filters by adapter type
   - Calls `onMeshDataImpl()` only if types match

2. **Outgoing Messages**:
   - Adapter calls `mesh_transmit_fn(message)` to send
   - Master node routes to appropriate destination
   - Non-master nodes broadcast to all peers

## Testing Your New Adapter

### 1. Compile and Upload

```bash
cd main
arduino --verify main.ino
arduino --upload main.ino
```

### 2. Monitor Serial Output

Connect to the master node's serial port to see:
- Adapter initialization messages
- Any error messages
- Mesh communication logs

### 3. Test Mesh Communication

1. **Send a test message** from the master node to trigger your adapter
2. **Verify the response** in the mesh network
3. **Check health reports** to confirm adapter type is correct

### 4. Test Configuration Changes

1. **Change adapter type** via serial command
2. **Verify EEPROM persistence** by restarting
3. **Confirm pin assignment** is correct for the new adapter

### 5. Common Issues and Solutions

| Issue | Solution |
|-------|----------|
| Compilation errors | Check includes and namespace usage |
| Adapter not responding | Verify pin configuration and hardware setup |
| Mesh messages not received | Check adapter type filtering logic |
| EEPROM not persisting | Use `EEPROM_Manager::getInstance()` methods — never call `EEPROM.write/commit` directly |
| Wrong pin being used | Verify default pin constant is set correctly |

### 6. Debugging Tips

1. **Add logging** to your adapter's methods:
   ```cpp
   // Logger::logln takes (tag, message, level). For formatted strings, use String():
   Logger::logln("TEMP", ("Temp_Adapter: Temperature read: " + String(_lastTemperature, 1)).c_str(), LogLevel::LOG_INFO);
   ```

2. **Check mesh message flow** by adding logging to `onMeshDataImpl`

3. **Verify pin configuration** by logging the pin value in constructor

4. **Test hardware independently** before integrating with mesh

## Example: Complete Temperature Adapter

Here's a complete example of a temperature sensor adapter:

**Files to create:**
- `main/src/Adapter/Temp_Adapter/Temp_Adapter.h`
- `main/src/Adapter/Temp_Adapter/Temp_Adapter.cpp`

**Changes to existing files:**
- Add `TEMP_ADAPTER = 4` to `Adapter.h`
- Add `TEMP_ADAPTER_DEFAULT_PIN = 34` to `AdapterFactory.h`
- Add case in `AdapterFactory::createAdapter`
- Include `Temp_Adapter.h` in `AdapterFactory.cpp`

**Testing:**
1. Upload the code
2. Monitor serial output for initialization
3. Send mesh message with `dataType = 4` to trigger temperature reading
4. Verify temperature data is broadcast back to the mesh

This guide should give you everything you need to add new adapters and test different configurations in the Lattice system!

## 2025 Update – Build-time Defaults & GPIO Helpers

* **DEFAULT_ADAPTER** – you no longer edit `main.ino` to pick a default adapter.  Instead change `lattice::config::DEFAULT_ADAPTER` in `project_config.h`.
* **GPIO helpers** – validation and `_initialized` bookkeeping are handled by `GpioOutput` / `GpioInput`.  New adapter drivers can simply call `GpioOutput::isValidOutputPin(pin)` instead of duplicating pin tables.
* **Seven-segment optional** – if your dev board lacks the TM1637 display set `ENABLE_SEVSEG_DISPLAY = false` in `project_config.h`; the ErrorCore will fall back to LED patterns.